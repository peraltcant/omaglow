#include "live.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include "spectrum.h"

namespace {
constexpr int sampleRate = 44100;
// Workspaces change by hand, so twice a second is quick enough.
constexpr int workspacePollMs = 500;
constexpr int systemPollMs = 1000;

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
    connect(&m_monitor, &QProcess::errorOccurred, this, [](QProcess::ProcessError) {
        qInfo("can't run dbus-monitor; notification layers stay dark");
    });
}

LiveFeed::~LiveFeed() {
    setNeeds(0);
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
    if (!socket.waitForConnected(200))
        return {};
    socket.write(request);
    socket.waitForBytesWritten(200);
    QByteArray reply;
    // Hyprland closes the connection after the reply.
    while (socket.waitForReadyRead(200))
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
