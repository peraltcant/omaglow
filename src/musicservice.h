#pragma once

#include <QList>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <memory>

#include "device.h"

// Shows what is playing as spectrum bars on the OLED: records the default
// output's monitor with parec, analyses the latest window ten times a second,
// and streams the bands to the keyboard. Runs headless as `omaglow music`,
// started by omaglow-music.service while the music display is chosen.
class MusicService : public QObject {
    Q_OBJECT

public:
    using Finder = std::function<DeviceNode()>;
    using Opener = std::function<Link *(const QString &path)>;

    // style: 1 bars, 2 mirrored bars, 3 line, as Armoury numbers them.
    explicit MusicService(int style, QObject *parent = nullptr);
    MusicService(int style, Finder finder, Opener opener, QObject *parent = nullptr);
    void start();

    // One frame; and samples as parec would deliver them. For tests too.
    void tick();
    void feed(const QList<float> &samples);

private:
    bool ensureLink();

    int m_style;
    Finder m_finder;
    Opener m_opener;
    int m_findTicks = 0;
    bool m_needHeader = true; // after connecting or when the keyboard slept
    QProcess m_recorder;
    QList<float> m_samples;
    QByteArray m_pending; // a sample split across reads
    std::unique_ptr<Link> m_link;
    QTimer m_timer;
    bool m_lastSilent = false;
    int m_silentTicks = 0;
};
