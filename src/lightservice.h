#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <functional>
#include <memory>

#include "design.h"
#include "device.h"
#include "live.h"

// Streams the profile's custom lighting frame by frame while its layers move;
// the keyboard can't draw them on its own. Runs headless as `omaglow lights`,
// from omaglow-lights.service, which the app starts while it is needed.
class LightService : public QObject {
    Q_OBJECT

public:
    // Wait after each report, as OpenRGB does: sent back to back they left
    // the keyboard stuck after minutes of streaming.
    static constexpr int ReportGapMs = 20;
    using Finder = std::function<DeviceNode()>;
    using Opener = std::function<Link *(const QString &path)>;

    explicit LightService(QObject *parent = nullptr);
    // For tests.
    LightService(Finder finder, Opener opener, QSettings *settings, QObject *parent = nullptr);

    void start();
    // Re-reads the lighting and starts or stops streaming it, finding the
    // keyboard again if it was away.
    void tick();

private:
    bool ensureLink();
    bool sendAll(const QList<QByteArray> &packets);
    void sendFrame();

    Finder m_finder;
    Opener m_opener;
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings;
    QTimer m_timer;
    std::unique_ptr<Link> m_link;
    QTimer m_frameTimer;
    QElapsedTimer m_clock;
    qint64 m_lastReport = -1; // when the last report went out, on m_clock
    qint64 m_startedAt = -1; // when the color cycle step was last sent
    Design::ProfileLighting m_streamed;
    QFileSystemWatcher m_watcher;
    LiveFeed m_live; // what live layers show, gathered while they are shown
    QTimer m_reload;
};
