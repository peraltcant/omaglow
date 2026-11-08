#include "live.h"

#include <QDir>
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
