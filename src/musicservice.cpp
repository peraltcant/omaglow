#include "musicservice.h"

#include <QCoreApplication>
#include <QtEndian>
#include <cstdio>

#include "protocol.h"
#include "spectrum.h"

namespace {
constexpr int sampleRate = 44100;
constexpr int frameMs = 100;          // what Armoury streams at
constexpr int silentEveryTicks = 10;  // a quiet room needs one frame a second
constexpr int findEveryTicks = 20;    // look for a missing keyboard every 2 s
}

MusicService::MusicService(int style, QObject *parent)
    : MusicService(style, [] { return Discovery::find(); },
                   [](const QString &path) -> Link * { return HidrawLink::openNode(path); }, parent) {}

MusicService::MusicService(int style, Finder finder, Opener opener, QObject *parent)
    : QObject(parent), m_style(qBound(1, style, 3)), m_finder(std::move(finder)), m_opener(std::move(opener)) {
    connect(&m_recorder, &QProcess::readyReadStandardOutput, this, [this] {
        QByteArray data = m_pending + m_recorder.readAllStandardOutput();
        const int whole = data.size() / 2 * 2;
        m_pending = data.mid(whole);
        QList<float> samples;
        samples.reserve(whole / 2);
        for (int i = 0; i < whole; i += 2)
            samples.append(qFromLittleEndian<qint16>(data.constData() + i) / 32768.0f);
        feed(samples);
    });
    // Without the recorder there is nothing to show; exit so the service
    // restarts and says why in the journal.
    connect(&m_recorder, &QProcess::finished, this, [this] {
        std::fprintf(stderr, "omaglow music: parec stopped: %s\n",
                     m_recorder.readAllStandardError().trimmed().constData());
        QCoreApplication::exit(1);
    });
    connect(&m_recorder, &QProcess::errorOccurred, this, [](QProcess::ProcessError) {
        std::fprintf(stderr, "omaglow music: can't run parec (pulseaudio-utils / pipewire-pulse)\n");
        // This can fire inside start(), before the event loop runs, where
        // exit() would do nothing; queue it instead.
        QMetaObject::invokeMethod(qApp, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    });
    m_timer.setInterval(frameMs);
    connect(&m_timer, &QTimer::timeout, this, &MusicService::tick);
}

void MusicService::start() {
    m_recorder.start("parec", {"--device=@DEFAULT_MONITOR@", "--format=s16le", "--rate=44100", "--channels=1",
                               "--latency-msec=50", "--raw"});
    m_timer.start();
}

void MusicService::feed(const QList<float> &samples) {
    m_samples.append(samples);
    if (m_samples.size() > Spectrum::WindowSize * 4)
        m_samples.remove(0, m_samples.size() - Spectrum::WindowSize);
}

bool MusicService::ensureLink() {
    if (!m_link) {
        if (m_findTicks++ % findEveryTicks != 0)
            return false;
        const DeviceNode node = m_finder();
        if (node.status != DeviceNode::Found)
            return false;
        m_link.reset(m_opener(node.path));
        if (!m_link)
            return false;
        m_needHeader = true;
    }
    if (m_needHeader) {
        // Spectrum only, no media info image: 208x64, no picture packets.
        QByteArray reply;
        {
            const DeviceLock lock;
            reply = m_link->transact(Protocol::musicSpectrumStart(m_style));
        }
        if (reply.isEmpty() || Protocol::isRejected(reply))
            return false; // asleep or out of range; try again next frame
        m_needHeader = false;
    }
    return true;
}

void MusicService::tick() {
    if (!ensureLink())
        return;
    const QByteArray bands = Spectrum::bands(m_samples, sampleRate);
    const bool silent = bands.count('\0') == bands.size();
    if (silent && m_lastSilent && ++m_silentTicks % silentEveryTicks != 0)
        return;
    m_lastSilent = silent;
    m_silentTicks = 0;
    QByteArray reply;
    {
        const DeviceLock lock;
        reply = m_link->transact(Protocol::musicBands(bands));
    }
    if (auto *hid = dynamic_cast<HidrawLink *>(m_link.get()); hid && hid->failed()) {
        m_link.reset(); // unplugged; look again and resend the header
        m_findTicks = 0;
        return;
    }
    // A sleeping keyboard forgets the music mode; set it again once it answers.
    if (reply.isEmpty() || Protocol::isRejected(reply))
        m_needHeader = true;
}
