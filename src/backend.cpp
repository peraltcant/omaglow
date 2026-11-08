#include "backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include "appinfo.h"
#include "design.h"
#include "filepicker.h"
#include "oledimage.h"
#include "portalfilepicker.h"

#include <cerrno>
#include <cstring>

namespace {
constexpr int pollIntervalMs = 2000;
constexpr int batteryEveryPolls = 15; // every 30 seconds
// A stuck keyboard is asked again this seldom, and only briefly, since the
// window waits for the answer.
constexpr int stuckEveryPolls = 3; // every 6 seconds
constexpr int stuckTimeoutMs = 150;
constexpr int replyTimeoutMs = 400;
}

Backend::Backend(QObject *parent)
    : Backend([] { return Discovery::find(); },
              [](const QString &path) -> Link * { return HidrawLink::openNode(path); },
              nullptr, nullptr, parent) {}

Backend::Backend(Finder finder, Opener opener, QSettings *settings, FilePicker *picker, QObject *parent)
    : QObject(parent), m_finder(std::move(finder)), m_opener(std::move(opener)), m_settings(settings),
      m_picker(picker) {
    m_hooksDirectory = QDir::homePath() + "/.config/omarchy/hooks/theme-set.d";
    m_mouseFinder = [] { return Razer::find(); };
    m_asusctl = [](const QStringList &args) {
        QProcess process;
        process.start("asusctl", args);
        if (!process.waitForFinished(3000)) {
            process.kill();
            return -1;
        }
        return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    };
    m_mouseOpener = [this]() -> Razer::FeaturePort * {
        const DeviceNode node = m_mouseFinder();
        return node.status == DeviceNode::Found ? Razer::HidrawFeaturePort::open(node.path) : nullptr;
    };
    // Which look is on follows every lighting change.
    connect(this, &Backend::lightingChanged, this, &Backend::looksChanged);
    if (!m_settings) {
        m_ownedSettings = std::make_unique<QSettings>(AppInfo::Name, AppInfo::Name);
        m_settings = m_ownedSettings.get();
    }
    if (!m_picker)
        m_picker = new PortalFilePicker(this);
    m_serviceRunner = [](const QStringList &args, QString *error) {
        QProcess process;
        process.start("systemctl", args);
        if (!process.waitForFinished(10000)) {
            *error = process.errorString();
            return -1;
        }
        *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    };
    connect(m_picker, &FilePicker::openSelected, this, [this](const QUrl &url) { uploadOled(url.toLocalFile()); });
    connect(m_picker, &FilePicker::failed, this, &Backend::setError);

    // Sliders change many times a second; send the last value once they rest.
    m_lightingTimer.setSingleShot(true);
    m_lightingTimer.setInterval(150);
    connect(&m_lightingTimer, &QTimer::timeout, this, &Backend::applyLighting);

    // The keyboard comes and goes (cable, dongle, a VM grabbing it), so keep
    // looking for it and notice when it disappears.
    m_pollTimer.setInterval(pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Backend::poll);
    connect(&m_upload, &QFutureWatcher<bool>::finished, this, [this] {
        m_busy = false;
        m_progress = m_upload.result() ? 1.0 : 0.0;
        emit busyChanged();
        if (!m_upload.result()) {
            setError(tr("The keyboard stopped answering while the picture was uploading."));
            disconnectDevice();
        }
    });

    load();
    connectDevice();
    QTimer::singleShot(0, this, &Backend::refreshMouse);
    m_pollTimer.start();
}

Backend::~Backend() {
    if (m_upload.isRunning())
        m_upload.waitForFinished();
    flush();
}

QVariantList Backend::effects() const {
    QVariantList list;
    for (int i = 0; i < Protocol::EffectCount; ++i) {
        const auto &info = Protocol::effectInfo(i);
        QVariantList directions;
        for (int d : info.directions)
            directions.append(d);
        list.append(QVariantMap{
            {"name", QCoreApplication::translate("Effects", info.name)},
            {"colors", info.colors},
            {"random", info.random},
            {"speed", info.speed},
            {"directions", directions},
            {"thickness", info.thickness},
        });
    }
    return list;
}

int Backend::direction() const {
    const auto &info = Protocol::effectInfo(m_lighting.effect);
    return info.directions.contains(m_lighting.direction) ? m_lighting.direction : info.defaultDirection;
}

void Backend::poll() {
    refreshMouse();
    if (m_busy)
        return;
    if (m_connection == Connected) {
        if (!QFileInfo::exists(m_node.path) || !m_link) {
            disconnectDevice();
            return;
        }
        if (m_asleep) {
            checkWake();
            return;
        }
        if (++m_pollTicks % batteryEveryPolls == 0)
            refreshBattery();
    } else if (m_connection == NotResponding) {
        if (++m_stuckPolls % stuckEveryPolls == 0)
            connectDevice();
    } else {
        connectDevice();
    }
}

void Backend::markNotResponding() {
    m_link.reset();
    m_firmware.clear();
    m_battery = -1;
    m_stuckPolls = 0;
    if (m_connection != NotResponding) {
        m_connection = NotResponding;
        emit connectionChanged();
    }
}

void Backend::connectDevice() {
    const DeviceNode node = m_finder();
    if (node.status == DeviceNode::Found) {
        Link *link = m_opener(node.path);
        if (!link) {
            m_node = node;
            m_connection = NoPermission;
            emit connectionChanged();
            return;
        }
        m_link.reset(link);
        m_node = node;
        const DeviceLock lock;
        // Each process reading the node gets its own copy of every reply, so
        // one question is enough; a keyboard already stuck gets a short wait.
        const bool retry = m_connection == NotResponding;
        m_link->setTimeoutMs(retry ? stuckTimeoutMs : replyTimeoutMs);
        m_firmware = Protocol::versionFromReply(m_link->transact(Protocol::queryVersion()), m_node.reportId > 0);
        if (m_node.reportId == 0 && m_firmware.isEmpty()) {
            // On the cable the keyboard always answers; silence means it is stuck.
            markNotResponding();
            return;
        }
        m_link->setTimeoutMs(replyTimeoutMs);
        m_battery = Protocol::batteryFromReply(m_link->transact(Protocol::queryBattery()));
        m_asleep = m_node.reportId > 0 && m_firmware.isEmpty();
        m_connection = Connected;
        emit connectionChanged();
        if (!m_asleep)
            resendDesign();
        return;
    }
    const int status = node.status == DeviceNode::NoPermission ? NoPermission : Disconnected;
    if (status != m_connection || node.path != m_node.path) {
        m_node = node;
        m_connection = status;
        emit connectionChanged();
    }
}

void Backend::checkWake() {
    if (!m_asleep || !m_link || m_busy)
        return;
    const DeviceLock lock;
    const QString firmware = Protocol::versionFromReply(m_link->transact(Protocol::queryVersion()), true);
    if (firmware.isEmpty())
        return;
    m_firmware = firmware;
    m_battery = Protocol::batteryFromReply(m_link->transact(Protocol::queryBattery()));
    m_asleep = false;
    if (m_error.contains("asleep"))
        setError({});
    emit connectionChanged();
    resendDesign();
}

void Backend::refreshBattery() {
    if (!m_link || m_busy)
        return;
    const DeviceLock lock;
    QByteArray reply = m_link->transact(Protocol::queryBattery());
    auto *hid = dynamic_cast<HidrawLink *>(m_link.get());
    // A reply can come late while the light stream keeps the keyboard busy,
    // so one silence on the cable is asked again before calling it stuck; a
    // link that failed (unplugged) isn't asked again.
    if (reply.isEmpty() && m_node.reportId == 0 && !(hid && hid->failed()))
        reply = m_link->transact(Protocol::queryBattery());
    if (hid && hid->failed()) {
        disconnectDevice();
        return;
    }
    if (m_node.reportId == 0 && reply.isEmpty()) {
        markNotResponding(); // on the cable silence means stuck, not asleep
        return;
    }
    if (m_node.reportId > 0 && Protocol::isRejected(reply)) {
        if (!m_asleep) {
            m_asleep = true;
            emit connectionChanged();
        }
        return;
    }
    const int battery = Protocol::batteryFromReply(reply);
    if (battery >= 0 && battery != m_battery) {
        m_battery = battery;
        emit connectionChanged();
    }
}

void Backend::disconnectDevice() {
    m_link.reset();
    m_asleep = false;
    m_firmware.clear();
    m_battery = -1;
    m_connection = Disconnected;
    emit connectionChanged();
}

void Backend::refresh() {
    if (m_busy)
        return;
    disconnectDevice();
    connectDevice();
}

bool Backend::send(const QList<QByteArray> &packets) {
    if (m_busy || m_connection != Connected || !m_link)
        return false;
    const DeviceLock lock; // a command list goes out whole, like a frame
    for (const QByteArray &packet : packets) {
        if (Protocol::isDirect(packet)) {
            if (!m_link->post(packet)) {
                disconnectDevice();
                return false;
            }
            continue;
        }
        const QByteArray reply = m_link->transact(packet);
        if (Protocol::isRejected(reply)) {
            if (m_node.reportId > 0) {
                // The receiver answers for a keyboard that is asleep or out of range.
                m_asleep = true;
                emit connectionChanged();
                setError(tr("The keyboard is asleep or out of range. Press a key to wake it, then try again."));
            } else {
                setError(tr("The keyboard rejected the command."));
            }
            return false;
        }
        if (reply.isEmpty()) {
            auto *hid = dynamic_cast<HidrawLink *>(m_link.get());
            if (!hid || hid->failed()) {
                disconnectDevice();
                return false;
            }
        }
    }
    return true;
}

void Backend::setError(const QString &message) {
    if (m_error == message)
        return;
    m_error = message;
    emit errorChanged();
}

void Backend::clearError() { setError({}); }

void Backend::scheduleLighting() {
    m_lightingDirty = true;
    store();
    emit lightingChanged();
    m_lightingTimer.start();
}

void Backend::applyLighting() {
    m_lightingTimer.stop();
    if (!m_lightingDirty && sender() == &m_lightingTimer)
        return;
    // The service reads the lighting from the settings file, and streams it
    // when it moves; then the app sends nothing, or the frames would fight.
    m_settings->sync();
    if (Design::animated(current())) {
        if (updateLightsService())
            m_lightingDirty = false;
        return;
    }
    QList<QByteArray> packets;
    if (!m_lightingOn) {
        packets = {Protocol::lightingOff(), Protocol::save()};
    } else if (m_custom) {
        // Direct mode isn't stored on the keyboard, so there is nothing to save.
        packets = designPackets();
    } else {
        packets = Protocol::lighting(m_lighting);
        packets.append(Protocol::save());
    }
    // The stream has to be gone first, or its next frame covers this.
    updateLightsService();
    if (send(packets))
        m_lightingDirty = false;
}

void Backend::flush() {
    if (m_lightingTimer.isActive())
        applyLighting();
}

void Backend::setLightingOn(bool on) {
    if (on == m_lightingOn)
        return;
    m_lightingOn = on;
    scheduleLighting();
}

QList<QByteArray> Backend::designPackets() const {
    return Design::packets(Design::render(current(), 0), m_lighting.brightness);
}

Design::ProfileLighting Backend::current() const {
    return {m_lightingOn, m_custom, m_lighting, m_design, m_layers};
}

void Backend::setCurrent(const Design::ProfileLighting &lighting) {
    m_lightingOn = lighting.on;
    m_custom = lighting.custom;
    m_lighting = lighting.lighting;
    m_design = lighting.keys;
    m_layers = lighting.layers;
}

// The keyboard goes back to its own effect after a switch or a power cycle,
// so the design is drawn again.
void Backend::resendDesign() {
    if (m_custom && m_lightingOn && m_connection == Connected && !Design::animated(current()))
        send(designPackets());
}

void Backend::setCustomLighting(bool on) {
    if (on == m_custom && (m_lightingOn || !on))
        return;
    m_custom = on;
    if (on)
        m_lightingOn = true;
    scheduleLighting();
}

QVariantMap Backend::design() const {
    QVariantMap map;
    for (auto it = m_design.begin(); it != m_design.end(); ++it)
        map.insert(QString::number(it.key()), it.value());
    return map;
}

void Backend::paintKeys(const QList<int> &codes, const QColor &color) {
    for (int code : codes)
        if (Keymap::find(code))
            m_design.insert(code, color.toRgb());
    m_custom = true;
    m_lightingOn = true;
    scheduleLighting();
}

void Backend::fillDesign(const QColor &color) {
    QList<int> codes;
    for (const auto &k : Keymap::layout())
        codes.append(k.code);
    paintKeys(codes, color);
}

void Backend::randomDesign() {
    for (const auto &k : Keymap::layout())
        m_design.insert(k.code, QColor::fromHsv(QRandomGenerator::global()->bounded(360), 255, 255));
    m_custom = true;
    m_lightingOn = true;
    scheduleLighting();
}

void Backend::clearDesign() {
    m_design.clear();
    scheduleLighting();
}

QVariantList Backend::layers() const {
    QVariantList list;
    for (const Design::Layer &layer : m_layers) {
        QVariantList keys;
        for (int code : layer.keys)
            keys.append(code);
        list.append(QVariantMap{{"name", layer.name}, {"effect", layer.effect}, {"speed", layer.speed},
                                {"color1", layer.color1}, {"color2", layer.color2},
                                {"randomColors", layer.randomColors}, {"keys", keys}});
    }
    return list;
}

bool Backend::animatedLighting() const {
    return Design::animated(current());
}

QStringList Backend::layerEffects() const {
    return {tr("Static"), tr("Breathing"), tr("Strobing"), tr("Color cycle"),
            tr("Rainbow"), tr("Starry night"), tr("Comet"), tr("Tide"), tr("Current"),
            tr("Fire"), tr("Plasma"), tr("Scanner"), tr("Fireflies"), tr("Heartbeat"), tr("Gradient"),
            tr("Music"), tr("Workspaces")};
}

// A layer edit shows the custom lighting; false for an index out of range.
bool Backend::editLayer(int index) {
    if (index < 0 || index >= m_layers.size())
        return false;
    m_custom = true;
    m_lightingOn = true;
    return true;
}

int Backend::addLayer(const QString &name) {
    Design::Layer layer;
    layer.name = name.trimmed().isEmpty() ? tr("Layer %1").arg(m_layers.size() + 1) : name.trimmed();
    layer.color2 = layer.color1; // one color until a second is picked
    m_layers.append(layer);
    editLayer(m_layers.size() - 1);
    scheduleLighting();
    return m_layers.size() - 1;
}

void Backend::removeLayer(int index) {
    if (!editLayer(index))
        return;
    m_layers.removeAt(index);
    scheduleLighting();
}

void Backend::moveLayer(int from, int to) {
    if (!editLayer(from) || to < 0 || to >= m_layers.size() || to == from)
        return;
    m_layers.move(from, to);
    scheduleLighting();
}

void Backend::renameLayer(int index, const QString &name) {
    if (name.trimmed().isEmpty() || !editLayer(index))
        return;
    m_layers[index].name = name.trimmed();
    scheduleLighting();
}

void Backend::setLayerEffect(int index, int effect) {
    if (!editLayer(index))
        return;
    m_layers[index].effect = qBound(0, effect, Design::LayerEffectCount - 1);
    scheduleLighting();
}

void Backend::setLayerSpeed(int index, int speed) {
    if (!editLayer(index))
        return;
    m_layers[index].speed = qBound(0, speed, 2);
    scheduleLighting();
}

void Backend::setLayerColor(int index, int which, const QColor &color) {
    if (!color.isValid() || !editLayer(index))
        return;
    (which == 1 ? m_layers[index].color2 : m_layers[index].color1) = color.toRgb();
    scheduleLighting();
}

void Backend::setLayerRandom(int index, bool on) {
    if (!editLayer(index))
        return;
    m_layers[index].randomColors = on;
    scheduleLighting();
}

void Backend::setLayerKeys(int index, const QList<int> &codes, bool add) {
    if (!editLayer(index))
        return;
    QList<int> &keys = m_layers[index].keys;
    for (int code : codes) {
        if (!Keymap::find(code))
            continue;
        if (add && !keys.contains(code))
            keys.append(code);
        else if (!add)
            keys.removeAll(code);
    }
    scheduleLighting();
}

QVariantMap Backend::frameAt(qint64 ms) const {
    QVariantMap map;
    const Design::Keys frame = Design::render(current(), ms, Design::demoLive(ms));
    for (auto it = frame.begin(); it != frame.end(); ++it)
        map.insert(QString::number(it.key()), it.value());
    return map;
}

void Backend::setEffect(int effect) {
    effect = qBound(0, effect, Protocol::EffectCount - 1);
    if (effect == m_lighting.effect && m_lightingOn && !m_custom)
        return;
    m_custom = false;
    m_lighting.effect = effect;
    m_lightingOn = true;
    scheduleLighting();
}

void Backend::setBrightness(int value) {
    value = qBound(0, value, 100);
    if (value == m_lighting.brightness)
        return;
    m_lighting.brightness = value;
    scheduleLighting();
}

void Backend::setSpeed(int value) {
    value = qBound(0, value, 2);
    if (value == m_lighting.speed)
        return;
    m_lighting.speed = value;
    scheduleLighting();
}

void Backend::setColorMode(int value) {
    value = qBound(0, value, 2);
    if (value == m_lighting.colorMode)
        return;
    m_lighting.colorMode = value;
    scheduleLighting();
}

void Backend::setColor1(const QColor &color) {
    if (color == m_lighting.color1)
        return;
    m_lighting.color1 = color;
    scheduleLighting();
}

void Backend::setColor2(const QColor &color) {
    if (color == m_lighting.color2)
        return;
    m_lighting.color2 = color;
    scheduleLighting();
}

void Backend::setDirection(int value) {
    if (value == m_lighting.direction)
        return;
    m_lighting.direction = value;
    scheduleLighting();
}

void Backend::setThickness(int value) {
    value = qBound(0, value, 2);
    if (value == m_lighting.thickness)
        return;
    m_lighting.thickness = value;
    scheduleLighting();
}

void Backend::setOledOn(bool on) {
    if (on == m_oledOn)
        return;
    // A dark display needs no time; pause the clock service with it.
    const QString live = m_oledClock ? AppInfo::service("clock") : m_oledMusic ? AppInfo::service("music") : QString();
    if (!live.isEmpty() && !on && !runService({"--user", "stop", live}, tr("Couldn't stop the display service: %1")))
        return;
    if (!send({Protocol::oledPower(on), Protocol::save()}))
        return;
    if (!live.isEmpty() && on)
        runService({"--user", "start", live}, tr("Couldn't start the display service: %1"));
    m_oledOn = on;
    store();
    emit oledChanged();
}

void Backend::setOledBrightness(int value) {
    value = qBound(0, value, 100);
    if (value == m_oledBrightness)
        return;
    if (!send({Protocol::oledBrightness(value), Protocol::save()}))
        return;
    m_oledBrightness = value;
    store();
    emit oledChanged();
}

bool Backend::runService(const QStringList &args, const QString &failure) {
    QString error;
    if (m_serviceRunner(args, &error) == 0)
        return true;
    setError(failure.arg(error.isEmpty() ? tr("systemctl failed") : error));
    return false;
}

bool Backend::switchClockService(bool on) {
    return runService({"--user", on ? "enable" : "disable", "--now", AppInfo::service("clock")},
                      on ? tr("Couldn't start the clock service: %1") : tr("Couldn't stop the clock service: %1"));
}

bool Backend::switchMusicService(bool on) {
    return runService({"--user", on ? "enable" : "disable", "--now", AppInfo::service("music")},
                      on ? tr("Couldn't start the music service: %1") : tr("Couldn't stop the music service: %1"));
}

bool Backend::stopLiveDisplays() {
    if (m_oledClock) {
        if (!switchClockService(false))
            return false;
        m_oledClock = false;
    }
    if (m_oledMusic) {
        if (!switchMusicService(false))
            return false;
        m_oledMusic = false;
    }
    return true;
}

void Backend::setOledMusic(bool on) {
    if (on == m_oledMusic)
        return;
    if (on && m_connection != Connected) {
        setError(tr("Connect the keyboard before showing music."));
        return;
    }
    if (on && !stopLiveDisplays())
        return;
    if (!switchMusicService(on))
        return;
    if (on)
        m_oledPreset = -1;
    m_oledMusic = on;
    store();
    emit oledChanged();
}

void Backend::setMusicStyle(int style) {
    style = qBound(1, style, 3);
    if (style == m_musicStyle)
        return;
    m_musicStyle = style;
    store();
    m_settings->sync(); // the service reads it when it restarts
    if (m_oledMusic)
        runService({"--user", "restart", AppInfo::service("music")}, tr("Couldn't restart the music service: %1"));
    emit oledChanged();
}

void Backend::syncClockService() {
    QString ignored;
    const bool enabled = m_serviceRunner({"--user", "is-enabled", "--quiet", AppInfo::service("clock")}, &ignored) == 0;
    const bool music = m_serviceRunner({"--user", "is-enabled", "--quiet", AppInfo::service("music")}, &ignored) == 0;
    bool clock = enabled;
    // Only one can own the display; if both were turned on outside the app,
    // keep the music and switch the clock off.
    if (clock && music && switchClockService(false))
        clock = false;
    if (clock == m_oledClock && music == m_oledMusic)
        return;
    m_oledClock = clock;
    m_oledMusic = music;
    store();
    emit oledChanged();
}

void Backend::setOledClock(bool on) {
    if (on == m_oledClock)
        return;
    if (on && m_connection != Connected) {
        setError(tr("Connect the keyboard before turning on the clock."));
        return;
    }
    if (on && m_oledMusic) {
        if (!switchMusicService(false))
            return;
        m_oledMusic = false;
    }
    // The service first: if it can't start, the keyboard must not be left
    // showing a clock that never moves.
    if (!switchClockService(on))
        return;
    if (on) {
        send({Protocol::clockTime(QDateTime::currentDateTime())}); // show it now, not at the next minute
        m_oledPreset = -1;
    }
    m_oledClock = on;
    store();
    emit oledChanged();
}

void Backend::chooseOledPreset(int index) {
    index = qBound(0, index, 5);
    if (!stopLiveDisplays())
        return;
    if (!send({Protocol::oledPreset(index), Protocol::save()}))
        return;
    m_oledPreset = index;
    m_oledFile.clear();
    m_previewFrames.clear();
    ++m_previewRevision;
    store();
    emit oledChanged();
}

void Backend::openOledImage() { m_picker->openImage(); }

QImage Backend::previewFrame(int index) const {
    if (m_previewFrames.isEmpty())
        return {};
    return OledImage::decodeFrame(m_previewFrames.value(qBound(0, index, m_previewFrames.size() - 1)));
}

void Backend::uploadOled(const QString &path) {
    if (m_busy)
        return;
    if (m_connection != Connected || !m_link) {
        setError(tr("Connect the keyboard before sending a picture."));
        return;
    }
    const OledImage::Animation animation = OledImage::load(path);
    if (!animation.error.isEmpty()) {
        setError(animation.error);
        return;
    }
    if (!stopLiveDisplays())
        return;

    m_previewFrames = animation.frames;
    m_oledFile = path;
    m_oledPreset = -1;
    ++m_previewRevision;
    store();
    emit oledChanged();

    QList<QByteArray> packets = Protocol::oledUpload(animation.frames, animation.delays,
        m_node.reportId > 0 ? Protocol::OledChunkReceiver : Protocol::OledChunk);
    packets.append(Protocol::save());
    m_busy = true;
    m_progress = 0;
    emit busyChanged();

    // A long GIF is thousands of acknowledged packets, so the upload runs off
    // the UI thread; nothing else touches the link while busy.
    Link *link = m_link.get();
    m_upload.setFuture(QtConcurrent::run([this, link, packets] {
        for (int i = 0; i < packets.size(); ++i) {
            QByteArray reply;
            {
                // Per report: a long upload still lets the light stream take turns.
                const DeviceLock lock;
                reply = link->transact(packets[i]);
            }
            if (reply.isEmpty() || Protocol::isRejected(reply))
                return false;
            if (i % 16 == 0 || i == packets.size() - 1) {
                const double progress = double(i + 1) / packets.size();
                QMetaObject::invokeMethod(this, [this, progress] {
                    m_progress = progress;
                    emit busyChanged();
                }, Qt::QueuedConnection);
            }
        }
        return true;
    }));
}

namespace {

const QList<QPair<int, const char *>> knobNames = {
    {Protocol::KnobVolume, QT_TRANSLATE_NOOP("Knob", "System volume")},
    {Protocol::KnobMedia, QT_TRANSLATE_NOOP("Knob", "Media track")},
    {Protocol::KnobKeyboardBrightness, QT_TRANSLATE_NOOP("Knob", "Keyboard brightness")},
    {Protocol::KnobOledBrightness, QT_TRANSLATE_NOOP("Knob", "Display brightness")},
    {Protocol::KnobEffect, QT_TRANSLATE_NOOP("Knob", "Switch lighting effect")},
    {Protocol::KnobCustom, QT_TRANSLATE_NOOP("Knob", "Custom")},
};

const QList<QPair<int, const char *>> slotNames = {
    {Protocol::KnobScrollUp, QT_TRANSLATE_NOOP("Knob", "Turn up")},
    {Protocol::KnobClick, QT_TRANSLATE_NOOP("Knob", "Press")},
    {Protocol::KnobScrollDown, QT_TRANSLATE_NOOP("Knob", "Turn down")},
};

QString actionText(const Keymap::Action &action) {
    if (Keymap::kindOf(action) == Keymap::KeyPress)
        return Keymap::labelOf(action);
    return QCoreApplication::translate("Keymap", Keymap::labelOf(action).toUtf8().constData());
}

QString encode(const Keymap::Action &a) { return QString("%1,%2").arg(a.code).arg(a.param); }

Keymap::Action decode(const QString &text) {
    const QStringList parts = text.split(',');
    if (parts.size() != 2)
        return {};
    return {parts[0].toInt() & 0xff, parts[1].toInt() & 0xff};
}

} // namespace

QVariantList Backend::keys() const {
    QVariantList list;
    for (const auto &k : Keymap::layout()) {
        const Keymap::Action action = m_remaps.value(k.code);
        list.append(QVariantMap{
            {"code", k.code}, {"label", QString::fromUtf8(k.label)},
            {"x", k.x}, {"y", k.y}, {"width", k.width},
            {"remapped", !action.isDefault()},
            {"actionCode", action.code}, {"actionParam", action.param},
            {"action", action.isDefault() ? QString() : actionText(action)},
        });
    }
    return list;
}

QVariantList Backend::keyChoices() const {
    QVariantList list;
    for (const auto &c : Keymap::specialChoices())
        list.append(QVariantMap{{"kind", int(c.kind)}, {"code", c.action.code}, {"param", c.action.param},
                                {"label", QCoreApplication::translate("Keymap", c.label)}});
    for (const auto &k : Keymap::layout())
        list.append(QVariantMap{{"kind", int(Keymap::KeyPress)}, {"code", k.code}, {"param", 0},
                                {"label", QString::fromUtf8(k.label)}});
    return list;
}

int Backend::remappedCount() const { return m_remaps.size(); }

void Backend::remap(int key, int code, int param) {
    if (!Keymap::find(key))
        return;
    const Keymap::Action action{code & 0xff, param & 0xff};
    if (action == m_remaps.value(key))
        return;
    if (!send({Protocol::remapKey(key, action.code, action.param), Protocol::save()}))
        return;
    if (action.isDefault())
        m_remaps.remove(key);
    else
        m_remaps.insert(key, action);
    store();
    emit keysChanged();
}

void Backend::resetKeys() {
    QList<QByteArray> packets;
    for (int code : Keymap::resetCodes())
        packets.append(Protocol::remapKey(code, 0xff, 0x00));
    packets.append(Protocol::save());
    if (!send(packets))
        return;
    m_remaps.clear();
    store();
    emit keysChanged();
}

QVariantList Backend::knobFunctions() const {
    QVariantList list;
    for (const auto &[id, name] : knobNames)
        list.append(QVariantMap{{"id", id}, {"name", QCoreApplication::translate("Knob", name)},
                                {"enabled", m_knob.value(id)}});
    return list;
}

QVariantList Backend::knobSlots() const {
    QVariantList list;
    for (const auto &[slot, name] : slotNames) {
        const Keymap::Action action = m_knobSlots.value(slot);
        list.append(QVariantMap{{"slot", slot}, {"name", QCoreApplication::translate("Knob", name)},
                                {"code", action.code}, {"param", action.param}, {"action", actionText(action)}});
    }
    return list;
}

void Backend::setKnobFunction(int function, bool enabled) {
    bool known = false;
    for (const auto &entry : knobNames)
        known = known || entry.first == function;
    if (!known || m_knob.value(function) == enabled)
        return;
    if (!send({Protocol::knobFunction(function, enabled), Protocol::save()}))
        return;
    m_knob.insert(function, enabled);
    store();
    emit knobChanged();
}

void Backend::setKnobSlot(int slot, int code, int param) {
    const Keymap::Action action{code & 0xff, param & 0xff};
    if (!m_knobSlots.contains(slot) || m_knobSlots.value(slot) == action)
        return;
    if (!send({Protocol::knobCustom(slot, action.code, action.param), Protocol::save()}))
        return;
    m_knobSlots.insert(slot, action);
    store();
    emit knobChanged();
}

void Backend::setPowerValue(int &field, int value, const QByteArray &packet) {
    if (value == field)
        return;
    if (!send({packet, Protocol::save()}))
        return;
    field = value;
    store();
    emit powerChanged();
}

void Backend::setSleepTimeout(int index) {
    index = index == Protocol::SleepNever ? index : qBound(0, index, 4);
    setPowerValue(m_sleepTimeout, index, Protocol::sleepTimeout(index));
}

void Backend::setBatteryAlert(int percent) {
    percent = qBound(10, percent, 50);
    setPowerValue(m_batteryAlert, percent, Protocol::batteryAlert(percent));
}

void Backend::setPowerSaving(int mode) {
    mode = qBound(0, mode, 2);
    setPowerValue(m_powerSaving, mode, Protocol::powerSaving(mode));
}

void Backend::setDebounceMs(int milliseconds) {
    milliseconds = qBound(15, (milliseconds / 5) * 5, 50);
    setPowerValue(m_debounceMs, milliseconds, Protocol::debounce(milliseconds));
}

// The default profile keeps the keys the app used before profiles existed,
// so earlier settings carry over; profiles 1-5 live under profile<N>/.
QString Backend::profilePrefix() const {
    return Design::profilePrefix(m_profile);
}

void Backend::setProfile(int profile) {
    profile = qBound(1, profile, int(Protocol::DefaultProfile));
    if (profile == m_profile)
        return;
    // A lighting change still waiting on its timer belongs to this profile;
    // send it before switching, or it would land on the next one.
    flush();
    if (!send({Protocol::switchProfile(profile), Protocol::save()}))
        return;
    storeProfile();
    m_profile = profile;
    loadProfile();
    store();
    // The keyboard keeps a design drawn in direct mode across the switch.
    // Stopping the stream first (systemctl waits for it), or its next frame
    // would cover the profile's lighting.
    updateLightsService();
    if (!Design::animated(current()))
        send(Design::packetsFor(current()));
    emit profileChanged();
    emit lightingChanged();
    emit keysChanged();
    emit knobChanged();
}

void Backend::loadProfile() {
    auto &s = *m_settings;
    const QString p = profilePrefix();
    const Design::ProfileLighting lighting = Design::load(s, m_profile);
    setCurrent(lighting);
    m_remaps.clear();
    for (const auto &k : Keymap::layout()) {
        const Keymap::Action action = decode(s.value(QString("%1keys/%2").arg(p).arg(k.code)).toString());
        if (!action.isDefault())
            m_remaps.insert(k.code, action);
    }
    for (const auto &entry : knobNames)
        m_knob.insert(entry.first, s.value(QString("%1knob/%2").arg(p).arg(entry.first),
                                           entry.first != Protocol::KnobCustom).toBool());
    for (const auto &entry : slotNames)
        m_knobSlots.insert(entry.first,
                           decode(s.value(QString("%1knob/slot%2").arg(p).arg(entry.first), "255,0").toString()));
}

void Backend::storeProfile() {
    auto &s = *m_settings;
    const QString p = profilePrefix();
    Design::storeAt(s, p, current());
    s.remove(p + "keys");
    for (auto it = m_remaps.begin(); it != m_remaps.end(); ++it)
        s.setValue(QString("%1keys/%2").arg(p).arg(it.key()), encode(it.value()));
    for (auto it = m_knob.begin(); it != m_knob.end(); ++it)
        s.setValue(QString("%1knob/%2").arg(p).arg(it.key()), it.value());
    for (auto it = m_knobSlots.begin(); it != m_knobSlots.end(); ++it)
        s.setValue(QString("%1knob/slot%2").arg(p).arg(it.key()), encode(it.value()));
}

void Backend::load() {
    auto &s = *m_settings;
    m_profile = qBound(1, s.value("device/profile", int(Protocol::DefaultProfile)).toInt(), int(Protocol::DefaultProfile));
    loadProfile();
    m_mouse = Design::loadMouse(s, QString());
    m_laptop = Laptop::decode(s.value("laptop/aura").toString(), &m_hasLaptop);
    m_followTheme = s.value("theme/follow", false).toBool();
    m_oledOn = s.value("oled/on", true).toBool();
    m_oledBrightness = qBound(0, s.value("oled/brightness", 100).toInt(), 100);
    m_oledPreset = s.value("oled/preset", -1).toInt();
    m_oledFile = s.value("oled/file").toString();
    m_oledClock = s.value("oled/clock", false).toBool();
    m_oledMusic = s.value("oled/music", false).toBool();
    m_musicStyle = qBound(1, s.value("oled/musicStyle", 1).toInt(), 3);
    m_sleepTimeout = s.value("power/sleep", 2).toInt();
    m_batteryAlert = qBound(10, s.value("power/alert", 20).toInt(), 50);
    m_powerSaving = qBound(0, s.value("power/saving", 1).toInt(), 2);
    m_debounceMs = qBound(15, s.value("power/debounce", 15).toInt(), 50);
    m_looks = Design::loadLooks(s);
    if (!m_oledFile.isEmpty()) {
        const auto animation = OledImage::load(m_oledFile);
        m_previewFrames = animation.frames;
    }
}

void Backend::store() {
    auto &s = *m_settings;
    s.setValue("device/profile", m_profile);
    storeProfile();
    s.setValue("oled/on", m_oledOn);
    s.setValue("oled/brightness", m_oledBrightness);
    s.setValue("oled/preset", m_oledPreset);
    s.setValue("oled/file", m_oledFile);
    s.setValue("oled/clock", m_oledClock);
    s.setValue("oled/music", m_oledMusic);
    s.setValue("oled/musicStyle", m_musicStyle);
    s.setValue("power/sleep", m_sleepTimeout);
    s.setValue("power/alert", m_batteryAlert);
    s.setValue("power/saving", m_powerSaving);
    s.setValue("power/debounce", m_debounceMs);
}

// The keyboard can't draw moving layers on its own, so a background service
// streams them while they are shown.
bool Backend::updateLightsService() {
    const bool wanted = Design::animated(current());
    if (wanted == m_serviceOn)
        return true;
    if (!runService({"--user", wanted ? "enable" : "disable", "--now", AppInfo::service("lights")},
                    wanted ? tr("Couldn't start the lighting service: %1") : tr("Couldn't stop the lighting service: %1")))
        return false;
    m_serviceOn = wanted;
    return true;
}

void Backend::syncLightsService() {
    QString ignored;
    // Running is what counts: a stopped service draws nothing, so moving
    // layers bring it back and still lighting stops a leftover one.
    m_serviceOn = m_serviceRunner({"--user", "is-active", "--quiet", AppInfo::service("lights")}, &ignored) == 0;
    updateLightsService();
}

QVariantList Backend::looks() const {
    QVariantList list;
    for (const auto &look : m_looks)
        list.append(QVariantMap{
            {"name", look.name},
            {"custom", look.lighting.custom},
            {"on", look.lighting.on},
            {"effect", look.lighting.lighting.effect},
            {"active", Design::sameLook(look.lighting, current())},
        });
    return list;
}

void Backend::storeLooks() {
    Design::storeLooks(*m_settings, m_looks);
    m_settings->sync();
    emit looksChanged();
}

void Backend::saveLook(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    Design::Look look{trimmed, current(), m_mouse};
    look.hasLaptop = m_hasLaptop;
    look.laptop = m_laptop;
    const int index = Design::findLook(m_looks, trimmed);
    if (index >= 0)
        m_looks[index] = look;
    else
        m_looks.append(look);
    storeLooks();
}

void Backend::applyLook(int index) {
    if (index < 0 || index >= m_looks.size())
        return;
    setCurrent(m_looks[index].lighting);
    scheduleLighting();
    const Design::MouseZones &mouse = m_looks[index].mouse;
    if (!mouse.isEmpty()) {
        for (auto it = mouse.begin(); it != mouse.end(); ++it)
            m_mouse.insert(it.key(), it.value());
        Design::storeMouse(*m_settings, QString(), m_mouse);
        emit mouseChanged();
        applyMouse(mouse);
    }
    if (m_looks[index].hasLaptop) {
        m_laptop = m_looks[index].laptop;
        m_hasLaptop = true;
        m_settings->setValue("laptop/aura", Laptop::encode(m_laptop));
        emit laptopChanged();
        applyLaptop(m_laptop);
    }
}

QVariantMap Backend::laptopAura() const {
    return {
        {"effect", m_laptop.effect},
        {"color1", m_laptop.color1},
        {"color2", m_laptop.color2},
        {"speed", m_laptop.speed},
        {"direction", m_laptop.direction},
        {"brightness", m_laptop.brightness},
    };
}

bool Backend::laptopAvailable() const {
    return !QStandardPaths::findExecutable("asusctl").isEmpty();
}

void Backend::setLaptopAura(int effect, const QColor &color1, const QColor &color2, int speed, int direction,
                            int brightness) {
    Laptop::Aura aura;
    aura.effect = qBound(0, effect, Laptop::EffectCount - 1);
    aura.color1 = color1.toRgb();
    aura.color2 = color2.toRgb();
    aura.speed = qBound(0, speed, 2);
    aura.direction = qBound(0, direction, 3);
    aura.brightness = qBound(0, brightness, 3);
    m_laptop = aura;
    m_hasLaptop = true;
    m_settings->setValue("laptop/aura", Laptop::encode(m_laptop));
    emit laptopChanged();
    applyLaptop(m_laptop);
}

void Backend::setFollowTheme(bool on) {
    // The hook azoth installed before the rename goes either way.
    QFile::remove(m_hooksDirectory + "/50-" + QString(AppInfo::LegacyName));
    const QString hook = m_hooksDirectory + "/50-" + QString(AppInfo::Name);
    if (on) {
        QDir().mkpath(m_hooksDirectory);
        QFile file(hook);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("#!/bin/sh\n"
                       "# Installed by Omaglow while \"Follow the Omarchy theme\" is on; Omaglow removes it\n"
                       "# when that is turned off. Paints the keyboard, mouse and laptop in the new\n"
                       "# theme's keyboard color. When the app is open it does this itself.\n"
                       "command -v omaglow >/dev/null 2>&1 && (omaglow theme-sync >/dev/null 2>&1 &)\n");
            file.close();
            file.setPermissions(file.permissions() | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        }
    } else {
        QFile::remove(hook);
    }
    if (on == m_followTheme)
        return;
    // Turning it on paints the theme again, even if it was the last one applied.
    if (on)
        m_settings->remove("theme/applied");
    m_followTheme = on;
    m_settings->setValue("theme/follow", on);
    emit followThemeChanged();
}

void Backend::applyThemeColor(const QColor &color) {
    if (!color.isValid() || m_settings->value("theme/applied").toString() == color.name())
        return;
    m_settings->setValue("theme/applied", color.name());
    m_custom = false;
    m_lightingOn = true;
    m_lighting.effect = Protocol::Static;
    m_lighting.color1 = color.toRgb();
    scheduleLighting();
    for (int led : {int(Razer::Logo), int(Razer::ScrollWheel)}) {
        const Razer::Zone was = m_mouse.value(led);
        setMouseZone(led, Razer::Static, color, color, false, m_mouse.contains(led) ? was.brightness : 100);
    }
    if (m_hasLaptop || laptopAvailable())
        setLaptopAura(Laptop::Static, color, color, m_laptop.speed, m_laptop.direction, qMax(1, m_laptop.brightness));
}

bool Backend::applyLaptop(const Laptop::Aura &aura) {
    bool ok = true;
    for (const QStringList &args : Laptop::commands(aura))
        ok = m_asusctl && m_asusctl(args) == 0 && ok;
    return ok;
}

void Backend::refreshMouse() {
    const int status = m_mouseFinder ? m_mouseFinder().status : DeviceNode::NotFound;
    if (status == m_mouseStatus)
        return;
    m_mouseStatus = status;
    emit mouseChanged();
}

QVariantMap Backend::mouseZones() const {
    QVariantMap map;
    for (auto it = m_mouse.begin(); it != m_mouse.end(); ++it)
        map.insert(QString::number(it.key()), QVariantMap{
            {"effect", it->effect},
            {"color1", it->color1},
            {"color2", it->color2},
            {"twoColors", it->twoColors},
            {"brightness", it->brightness},
        });
    return map;
}

void Backend::setMouseZone(int led, int effect, const QColor &color1, const QColor &color2, bool twoColors,
                           int brightness) {
    if (led != Razer::Logo && led != Razer::ScrollWheel)
        return;
    Razer::Zone zone;
    zone.effect = qBound(0, effect, Razer::EffectCount - 1);
    zone.color1 = color1.toRgb();
    zone.color2 = color2.toRgb();
    zone.twoColors = twoColors;
    zone.brightness = qBound(0, brightness, 100);
    m_mouse.insert(led, zone);
    Design::storeMouse(*m_settings, QString(), m_mouse);
    emit mouseChanged();
    applyMouse({{led, zone}});
}

// The mouse is opened for each change: it comes and goes, and nothing streams to it.
bool Backend::applyMouse(const Design::MouseZones &zones) {
    std::unique_ptr<Razer::FeaturePort> port(m_mouseOpener ? m_mouseOpener() : nullptr);
    if (!port)
        return false;
    Razer::Mouse mouse(port.get());
    bool ok = true;
    for (auto it = zones.begin(); it != zones.end(); ++it)
        ok = mouse.apply(it.key(), it.value()) && ok;
    return ok;
}

void Backend::deleteLook(int index) {
    if (index < 0 || index >= m_looks.size())
        return;
    m_looks.removeAt(index);
    storeLooks();
}
