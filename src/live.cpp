#include "live.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include "keymap.h"

#include "spectrum.h"

namespace {
constexpr int sampleRate = 44100;
// Workspaces change by hand, so twice a second is quick enough.
constexpr int workspacePollMs = 500;
constexpr int systemPollMs = 1000;
// Presses older than this have faded at any speed and are forgotten.
constexpr qint64 pressMemoryMs = 6000;
const QList<QByteArray> azothProducts{"1A83", "1A85", "1ACE"};

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}
}

LiveFeed::LiveFeed(QObject *parent) : QObject(parent) {
    connect(&m_recorder, &QProcess::readyReadStandardOutput, this, [this] {
        QByteArray data = m_pending + m_recorder.readAllStandardOutput();
        const int whole = data.size() / 2 * 2;
        m_pending = data.mid(whole);
        for (int i = 0; i < whole; i += 2)
            m_samples.append(qFromLittleEndian<qint16>(data.constData() + i) / 32768.0f);
        if (m_samples.size() > Spectrum::WindowSize * 4)
            m_samples.remove(0, m_samples.size() - Spectrum::WindowSize);
    });
    // Bars from a recorder that stopped would freeze; show none instead.
    connect(&m_recorder, &QProcess::finished, this, [this] {
        m_samples.clear();
        m_pending.clear();
    });
    connect(&m_recorder, &QProcess::errorOccurred, this, [](QProcess::ProcessError) {
        qInfo("can't run parec (pulseaudio-utils / pipewire-pulse); music layers stay dark");
    });
    m_workspaceTimer.setInterval(workspacePollMs);
    connect(&m_workspaceTimer, &QTimer::timeout, this, &LiveFeed::pollWorkspaces);
    m_systemTimer.setInterval(systemPollMs);
    connect(&m_systemTimer, &QTimer::timeout, this, &LiveFeed::pollSystem);
    // One line per message on the bus that matches; only the header line
    // (member=Notify) counts, the arguments that follow are skipped.
    connect(&m_monitor, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_monitor.canReadLine()) {
            const QByteArray line = m_monitor.readLine();
            if (line.startsWith("method call") && line.contains("member=Notify"))
                m_notifiedAt = m_clockNow ? m_clockNow() : 0;
        }
    });
    m_keyInputRetry.setInterval(5000);
    connect(&m_keyInputRetry, &QTimer::timeout, this, [this] {
        if (m_keyInputs.empty())
            openKeyInputs();
    });
    connect(&m_monitor, &QProcess::errorOccurred, this, [](QProcess::ProcessError) {
        qInfo("can't run dbus-monitor; notification layers stay dark");
    });
}

LiveFeed::~LiveFeed() {
    setNeeds(0);
    closeKeyInputs();
}

void LiveFeed::setNeeds(int needs) {
    const bool music = needs & Design::NeedsMusic, workspaces = needs & Design::NeedsWorkspaces;
    if (music && m_recorder.state() == QProcess::NotRunning) {
        m_recorder.start("parec", {"--device=@DEFAULT_MONITOR@", "--format=s16le", "--rate=44100",
                                   "--channels=1", "--latency-msec=50", "--raw"});
    } else if (!music && m_recorder.state() != QProcess::NotRunning) {
        m_recorder.kill();
        m_recorder.waitForFinished(500);
        m_samples.clear();
        m_pending.clear();
    }
    if (workspaces && !m_workspaceTimer.isActive()) {
        pollWorkspaces();
        m_workspaceTimer.start();
    } else if (!workspaces && m_workspaceTimer.isActive()) {
        m_workspaceTimer.stop();
        m_busy.clear();
        m_active = 0;
    }
    const bool system = needs & Design::NeedsSystem, notifications = needs & Design::NeedsNotifications;
    if (system && !m_systemTimer.isActive()) {
        m_lastCpu = cpuTimes(readFile("/proc/stat"));
        pollSystem();
        m_systemTimer.start();
    } else if (!system && m_systemTimer.isActive()) {
        m_systemTimer.stop();
        m_cpu = m_temperature = m_battery = -1;
    }
    if (notifications && m_monitor.state() == QProcess::NotRunning) {
        m_monitor.start("dbus-monitor", {"--session", "type='method_call',interface='org.freedesktop.Notifications',member='Notify'"});
    } else if (!notifications && m_monitor.state() != QProcess::NotRunning) {
        m_monitor.kill();
        m_monitor.waitForFinished(500);
        m_notifiedAt = -1;
    }
    const bool typing = needs & Design::NeedsTyping;
    if (typing && !m_keyInputRetry.isActive()) {
        openKeyInputs();
        m_keyInputRetry.start();
    } else if (!typing && m_keyInputRetry.isActive()) {
        m_keyInputRetry.stop();
        closeKeyInputs();
    }
    m_needs = needs;
}

Design::Live LiveFeed::snapshot() const {
    Design::Live live;
    if (m_samples.size() >= Spectrum::WindowSize) {
        const QByteArray bands = Spectrum::bands(m_samples.mid(m_samples.size() - Spectrum::WindowSize), sampleRate);
        for (char b : bands)
            live.spectrum.append(quint8(b));
    }
    live.busyWorkspaces = m_busy;
    live.activeWorkspace = m_active;
    live.cpu = m_cpu;
    live.temperature = m_temperature;
    live.battery = m_battery;
    live.notifiedAt = m_notifiedAt;
    live.pressedAt = m_pressedAt;
    return live;
}

QSet<int> LiveFeed::busyWorkspaces(const QByteArray &json) {
    QSet<int> busy;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        if (o.value("id").toInt() > 0 && o.value("windows").toInt() > 0)
            busy.insert(o.value("id").toInt());
    }
    return busy;
}

int LiveFeed::activeWorkspace(const QByteArray &json) {
    return QJsonDocument::fromJson(json).object().value("id").toInt();
}

QString LiveFeed::hyprlandSocket() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/";
    const QString own = runtime + qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + "/.socket.sock";
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && QFileInfo::exists(own))
        return own;
    // The service may have started before this Hyprland did.
    const QFileInfoList dirs = QDir(runtime).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &dir : dirs)
        if (QFileInfo::exists(dir.filePath() + "/.socket.sock"))
            return dir.filePath() + "/.socket.sock";
    return {};
}

QByteArray LiveFeed::askHyprland(const QByteArray &request) const {
    const QString path = hyprlandSocket();
    if (path.isEmpty())
        return {};
    QLocalSocket socket;
    socket.connectToServer(path);
    // A local socket answers in a few milliseconds; waiting longer would
    // hold up the frames, which run on the same thread.
    if (!socket.waitForConnected(50))
        return {};
    socket.write(request);
    socket.waitForBytesWritten(50);
    QByteArray reply;
    // Hyprland closes the connection after the reply.
    while (socket.waitForReadyRead(50))
        reply += socket.readAll();
    return reply + socket.readAll();
}

void LiveFeed::pollWorkspaces() {
    m_busy = busyWorkspaces(askHyprland("j/workspaces"));
    m_active = activeWorkspace(askHyprland("j/activeworkspace"));
}

LiveFeed::CpuTimes LiveFeed::cpuTimes(const QByteArray &stat) {
    // cpu user nice system idle iowait irq softirq steal ...
    const QList<QByteArray> fields = stat.left(stat.indexOf('\n')).simplified().split(' ');
    CpuTimes times;
    if (fields.size() < 5 || fields.first() != "cpu")
        return times;
    for (int i = 1; i < fields.size() && i <= 8; ++i) {
        const qint64 v = fields.at(i).toLongLong();
        times.total += v;
        if (i != 4 && i != 5) // idle and iowait
            times.busy += v;
    }
    return times;
}

double LiveFeed::cpuBusy(const CpuTimes &before, const CpuTimes &after) {
    const qint64 total = after.total - before.total;
    return total > 0 ? qBound(0.0, double(after.busy - before.busy) / total, 1.0) : -1;
}

double LiveFeed::readTemperature(const QString &sysRoot) {
    const QDir hwmon(sysRoot + "/class/hwmon");
    for (const QString &dir : hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QByteArray name = readFile(hwmon.filePath(dir + "/name"));
        if (name != "coretemp" && name != "k10temp" && name != "zenpower" && name != "cpu_thermal")
            continue;
        bool ok = false;
        const double milli = readFile(hwmon.filePath(dir + "/temp1_input")).toDouble(&ok);
        if (ok)
            return milli / 1000;
    }
    return -1;
}

double LiveFeed::readBattery(const QString &sysRoot) {
    const QDir supplies(sysRoot + "/class/power_supply");
    for (const QString &dir : supplies.entryList({"BAT*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        bool ok = false;
        const double charge = readFile(supplies.filePath(dir + "/capacity")).toDouble(&ok);
        if (ok)
            return charge;
    }
    return -1;
}

void LiveFeed::pollSystem() {
    const CpuTimes now = cpuTimes(readFile("/proc/stat"));
    m_cpu = cpuBusy(m_lastCpu, now);
    m_lastCpu = now;
    m_temperature = readTemperature();
    m_battery = readBattery();
}

QSet<int> LiveFeed::pressedUsages(const QByteArray &report) {
    QSet<int> usages;
    if (report.size() == 8) {
        for (int bit = 0; bit < 8; ++bit)
            if (quint8(report.at(0)) & (1 << bit))
                usages.insert(0xe0 + bit);
        // 1-3 are error codes (too many keys), not keys.
        for (int i = 2; i < 8; ++i)
            if (quint8(report.at(i)) >= 4)
                usages.insert(quint8(report.at(i)));
        return usages;
    }
    for (int i = 0; i < report.size() * 8; ++i)
        if (quint8(report.at(i / 8)) & (1 << (i % 8)))
            usages.insert(i);
    return usages;
}

QStringList LiveFeed::keyboardInputNodes(const QString &sysRoot, const QString &devRoot) {
    QStringList nodes;
    const QDir hidraw(sysRoot + "/class/hidraw");
    for (const QString &name : hidraw.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QByteArray uevent = readFile(hidraw.filePath(name + "/device/uevent")).toUpper();
        const int at = uevent.indexOf("HID_ID=");
        const QList<QByteArray> id = uevent.mid(at + 7, uevent.indexOf('\n', at) - at - 7).split(':');
        if (at < 0 || id.size() != 3 || !id[1].endsWith("0B05") || !azothProducts.contains(id[2].right(4)))
            continue;
        QFile file(hidraw.filePath(name + "/device/report_descriptor"));
        const QByteArray descriptor = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        // Generic desktop, keyboard, application collection; report ids would
        // shift the bytes, and the Azoth's key interfaces have none.
        if (descriptor.startsWith(QByteArray::fromHex("05010906a101")) && !descriptor.contains('\x85'))
            nodes.append(devRoot + "/" + name);
    }
    return nodes;
}

void LiveFeed::openKeyInputs() {
    closeKeyInputs();
    for (const QString &path : keyboardInputNodes()) {
        // Read only: each open file gets its own copy of every report, so the
        // keyboard keeps typing as usual.
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        KeyInput input{fd, std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read), {}};
        connect(input.notifier.get(), &QSocketNotifier::activated, this, [this, fd] { readKeys(fd); });
        m_keyInputs.push_back(std::move(input));
    }
}

void LiveFeed::closeKeyInputs() {
    for (KeyInput &input : m_keyInputs) {
        input.notifier.reset();
        ::close(input.fd);
    }
    m_keyInputs.clear();
    m_pressedAt.clear();
}

void LiveFeed::readKeys(int fd) {
    auto input = std::find_if(m_keyInputs.begin(), m_keyInputs.end(), [fd](const KeyInput &k) { return k.fd == fd; });
    if (input == m_keyInputs.end())
        return;
    char buffer[64];
    const qint64 now = m_clockNow ? m_clockNow() : 0;
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) {
            // Gone (unplugged): try again on the next retry.
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                QMetaObject::invokeMethod(this, [this] { closeKeyInputs(); }, Qt::QueuedConnection);
            break;
        }
        const QSet<int> held = pressedUsages(QByteArray(buffer, int(n)));
        for (int usage : held)
            if (!input->held.contains(usage))
                if (const int code = Keymap::codeOfUsage(usage); code >= 0)
                    m_pressedAt.insert(code, now);
        input->held = held;
    }
    for (auto it = m_pressedAt.begin(); it != m_pressedAt.end();)
        it = now - it.value() > pressMemoryMs ? m_pressedAt.erase(it) : std::next(it);
}
