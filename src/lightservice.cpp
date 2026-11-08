#include "lightservice.h"

#include <QDebug>
#include <QFileInfo>
#include <QThread>

#include "appinfo.h"
#include "protocol.h"

namespace {
// A returning keyboard and settings the watcher missed are picked up this often.
constexpr int tickMs = 5000;
// Other commands, like the app's battery check, put the keyboard back to
// drawing a single frame; repeating the color cycle step this often heals it.
constexpr qint64 restartMs = 1000;
}

LightService::LightService(QObject *parent)
    : LightService([] { return Discovery::find(); },
                   [](const QString &path) -> Link * { return HidrawLink::openNode(path); }, nullptr, parent) {}

LightService::LightService(Finder finder, Opener opener, QSettings *settings, QObject *parent)
    : QObject(parent), m_finder(std::move(finder)), m_opener(std::move(opener)), m_settings(settings) {
    if (!m_settings) {
        m_ownedSettings = std::make_unique<QSettings>(AppInfo::Name, AppInfo::Name);
        m_settings = m_ownedSettings.get();
    }
    connect(&m_timer, &QTimer::timeout, this, &LightService::tick);
    connect(&m_frameTimer, &QTimer::timeout, this, &LightService::sendFrame);
    m_frameTimer.setInterval(Design::FrameMs);
    m_clock.start();
    // The app saves the settings as it edits; pick changes up right away.
    // QSettings replaces the file, so the folder is what is watched.
    m_reload.setSingleShot(true);
    m_reload.setInterval(150);
    connect(&m_reload, &QTimer::timeout, this, &LightService::tick);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reload, qOverload<>(&QTimer::start));
    m_watcher.addPath(QFileInfo(m_settings->fileName()).absolutePath());
}

void LightService::start() {
    tick();
    m_timer.start(tickMs);
}

bool LightService::ensureLink() {
    if (m_link)
        return true;
    const DeviceNode node = m_finder();
    if (node.status != DeviceNode::Found)
        return false;
    m_link.reset(m_opener(node.path));
    return bool(m_link);
}

// A frame's last report is the one the keyboard answers.
bool LightService::sendAll(const QList<QByteArray> &packets) {
    for (int i = 0; i < packets.size(); ++i) {
        // A turn per report, not per frame: with the pauses a frame takes about
        // 140 ms, and holding it that long would keep the app waiting.
        const DeviceLock lock;
        bool ok;
        const qint64 since = m_lastReport < 0 ? ReportGapMs : m_clock.elapsed() - m_lastReport;
        if (since < ReportGapMs)
            QThread::msleep(ReportGapMs - since);
        m_lastReport = m_clock.elapsed();
        if (i + 1 < packets.size()) {
            ok = m_link->post(packets[i]);
        } else {
            const QByteArray reply = m_link->transact(packets[i]);
            // Behind the receiver, ff aa means the keyboard is asleep or out of
            // range: nothing was drawn, so wait for the next check.
            ok = !reply.isEmpty() && !(Protocol::isDirect(packets[i]) && Protocol::isRejected(reply));
        }
        if (!ok) {
            qInfo() << "keyboard didn't take a frame";
            if (auto *hid = dynamic_cast<HidrawLink *>(m_link.get()); !hid || hid->failed())
                m_link.reset();
            return false;
        }
    }
    return true;
}

void LightService::tick() {
    m_settings->sync();
    const int profile = qBound(1, m_settings->value("device/profile", int(Protocol::DefaultProfile)).toInt(),
                               int(Protocol::DefaultProfile));
    m_streamed = Design::load(*m_settings, profile); // edits show on the next frame
    if (!Design::animated(m_streamed)) {
        // The app draws still lighting itself.
        if (m_frameTimer.isActive())
            qInfo() << "stopped streaming";
        m_frameTimer.stop();
        return;
    }
    if (m_frameTimer.isActive() || !ensureLink())
        return;
    qInfo().noquote() << "streaming profile" << profile;
    m_startedAt = -1;
    m_frameTimer.start();
    sendFrame();
}

void LightService::sendFrame() {
    if (!m_link)
        return;
    const qint64 now = m_clock.elapsed();
    if (m_startedAt < 0 || now - m_startedAt >= restartMs) {
        if (!sendAll({Protocol::directStart()})) {
            m_frameTimer.stop();
            return;
        }
        m_startedAt = now;
    }
    const QList<QByteArray> packets =
        Design::packets(Design::render(m_streamed, m_clock.elapsed()), m_streamed.lighting.brightness);
    // Try again from the next check, when the keyboard may be back.
    if (!sendAll(packets))
        m_frameTimer.stop();
}
