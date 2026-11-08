#pragma once

#include <QColor>
#include <QHash>
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

#include "design.h"
#include "device.h"
#include "keymap.h"
#include "protocol.h"

class FilePicker;

// The keyboard and everything the window can change on it. Settings are
// remembered on disk because the keyboard can't report its own state back.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int connection READ connection NOTIFY connectionChanged)
    Q_PROPERTY(QString devicePath READ devicePath NOTIFY connectionChanged)
    Q_PROPERTY(bool wireless READ wireless NOTIFY connectionChanged)
    Q_PROPERTY(bool canSendPictures READ canSendPictures NOTIFY connectionChanged)
    Q_PROPERTY(bool asleep READ asleep NOTIFY connectionChanged)
    Q_PROPERTY(QString firmware READ firmware NOTIFY connectionChanged)
    Q_PROPERTY(int battery READ battery NOTIFY connectionChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY busyChanged)

    Q_PROPERTY(QVariantList effects READ effects CONSTANT)
    Q_PROPERTY(bool lightingOn READ lightingOn WRITE setLightingOn NOTIFY lightingChanged)
    Q_PROPERTY(int effect READ effect WRITE setEffect NOTIFY lightingChanged)
    Q_PROPERTY(int brightness READ brightness WRITE setBrightness NOTIFY lightingChanged)
    Q_PROPERTY(int speed READ speed WRITE setSpeed NOTIFY lightingChanged)
    Q_PROPERTY(int colorMode READ colorMode WRITE setColorMode NOTIFY lightingChanged)
    Q_PROPERTY(QColor color1 READ color1 WRITE setColor1 NOTIFY lightingChanged)
    Q_PROPERTY(QColor color2 READ color2 WRITE setColor2 NOTIFY lightingChanged)
    Q_PROPERTY(int direction READ direction WRITE setDirection NOTIFY lightingChanged)
    Q_PROPERTY(int thickness READ thickness WRITE setThickness NOTIFY lightingChanged)
    // A design painted key by key, drawn in direct mode instead of an effect.
    Q_PROPERTY(bool customLighting READ customLighting WRITE setCustomLighting NOTIFY lightingChanged)
    Q_PROPERTY(QVariantMap design READ design NOTIFY lightingChanged)
    // Groups of keys with an effect each, drawn over the design, as in Aura
    // Creator. Each is {name, effect, speed, color1, color2, randomColors, keys}.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY lightingChanged)
    // Moving layers are streamed by the background service.
    Q_PROPERTY(bool animatedLighting READ animatedLighting NOTIFY lightingChanged)
    Q_PROPERTY(QStringList layerEffects READ layerEffects CONSTANT)

    Q_PROPERTY(bool oledOn READ oledOn WRITE setOledOn NOTIFY oledChanged)
    Q_PROPERTY(int oledBrightness READ oledBrightness WRITE setOledBrightness NOTIFY oledChanged)
    Q_PROPERTY(int oledPreset READ oledPreset NOTIFY oledChanged)
    Q_PROPERTY(QString oledFile READ oledFile NOTIFY oledChanged)
    Q_PROPERTY(int oledFrames READ oledFrames NOTIFY oledChanged)
    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY oledChanged)
    Q_PROPERTY(bool oledClock READ oledClock WRITE setOledClock NOTIFY oledChanged)
    Q_PROPERTY(bool oledMusic READ oledMusic WRITE setOledMusic NOTIFY oledChanged)
    Q_PROPERTY(int musicStyle READ musicStyle WRITE setMusicStyle NOTIFY oledChanged)

    Q_PROPERTY(QVariantList keys READ keys NOTIFY keysChanged)
    Q_PROPERTY(QVariantList keyChoices READ keyChoices CONSTANT)
    Q_PROPERTY(int remappedCount READ remappedCount NOTIFY keysChanged)
    Q_PROPERTY(QVariantList knobFunctions READ knobFunctions NOTIFY knobChanged)
    Q_PROPERTY(QVariantList knobSlots READ knobSlots NOTIFY knobChanged)

    Q_PROPERTY(int profile READ profile WRITE setProfile NOTIFY profileChanged)
    // Saved lightings, each {name, custom, effect}.
    Q_PROPERTY(QVariantList looks READ looks NOTIFY looksChanged)
    // The Razer mouse's lighting by LED ("4" logo, "1" scroll wheel):
    // {effect, color1, color2, twoColors, brightness}.
    Q_PROPERTY(QVariantMap mouseZones READ mouseZones NOTIFY mouseChanged)
    // DeviceNode::Status of the Razer mouse: not found, no permission, found.
    Q_PROPERTY(int mouseStatus READ mouseStatus NOTIFY mouseChanged)
    // The laptop keyboard through asusctl: {effect, color1, color2, speed,
    // direction, brightness}, and whether asusctl is installed.
    Q_PROPERTY(QVariantMap laptopAura READ laptopAura NOTIFY laptopChanged)
    Q_PROPERTY(bool laptopAvailable READ laptopAvailable CONSTANT)
    // Paint the keyboard, mouse and laptop in the Omarchy theme's keyboard color.
    Q_PROPERTY(bool followTheme READ followTheme WRITE setFollowTheme NOTIFY followThemeChanged)
    Q_PROPERTY(int sleepTimeout READ sleepTimeout WRITE setSleepTimeout NOTIFY powerChanged)
    Q_PROPERTY(int batteryAlert READ batteryAlert WRITE setBatteryAlert NOTIFY powerChanged)
    Q_PROPERTY(int powerSaving READ powerSaving WRITE setPowerSaving NOTIFY powerChanged)
    Q_PROPERTY(int debounceMs READ debounceMs WRITE setDebounceMs NOTIFY powerChanged)

public:
    // NotResponding: found on the cable but it doesn't answer (stuck until replugged).
    enum Connection { Disconnected, NoPermission, Connected, NotResponding };
    Q_ENUM(Connection)

    using Finder = std::function<DeviceNode()>;
    using Opener = std::function<Link *(const QString &path)>;

    explicit Backend(QObject *parent = nullptr);
    Backend(Finder finder, Opener opener, QSettings *settings, FilePicker *picker, QObject *parent = nullptr);
    ~Backend() override;

    int connection() const { return m_connection; }
    QString devicePath() const { return m_node.path; }
    bool wireless() const { return !m_node.productId.isEmpty() && m_node.productId != "1A83"; }
    // Both the cable and the Omni receiver carry pictures now (59-byte chunks there).
    bool canSendPictures() const { return true; }
    QString firmware() const { return m_firmware; }
    // Behind the receiver a sleeping keyboard doesn't answer; the receiver
    // still does, so the connection stays up while the keyboard sleeps.
    bool asleep() const { return m_asleep; }
    Q_INVOKABLE void checkWake();
    // The regular check (every 2 s): connection, wake-up, battery, the mouse.
    void poll();
    // Reads the battery; behind the receiver an unanswered query means asleep.
    void refreshBattery();
    int battery() const { return m_battery; }
    QString error() const { return m_error; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }

    QVariantList effects() const;
    bool lightingOn() const { return m_lightingOn; }
    int effect() const { return m_lighting.effect; }
    int brightness() const { return m_lighting.brightness; }
    int speed() const { return m_lighting.speed; }
    int colorMode() const { return m_lighting.colorMode; }
    QColor color1() const { return m_lighting.color1; }
    QColor color2() const { return m_lighting.color2; }
    int direction() const;
    int thickness() const { return m_lighting.thickness; }

    void setLightingOn(bool on);
    void setEffect(int effect);
    void setBrightness(int value);
    void setSpeed(int value);
    void setColorMode(int value);
    void setColor1(const QColor &color);
    void setColor2(const QColor &color);
    void setDirection(int value);
    void setThickness(int value);

    bool customLighting() const { return m_custom; }
    void setCustomLighting(bool on);
    // Key code (as text) to color; keys left out stay dark.
    QVariantMap design() const;
    // Painting turns the custom design on.
    Q_INVOKABLE void paintKeys(const QList<int> &codes, const QColor &color);
    Q_INVOKABLE void fillDesign(const QColor &color);
    Q_INVOKABLE void randomDesign();
    Q_INVOKABLE void clearDesign();

    QVariantList layers() const;
    bool animatedLighting() const;
    QStringList layerEffects() const;
    // Editing a layer turns the custom design on. Returns the new layer's index.
    Q_INVOKABLE int addLayer(const QString &name);
    Q_INVOKABLE void removeLayer(int index);
    Q_INVOKABLE void moveLayer(int from, int to);
    Q_INVOKABLE void renameLayer(int index, const QString &name);
    Q_INVOKABLE void setLayerEffect(int index, int effect);
    Q_INVOKABLE void setLayerSpeed(int index, int speed);
    // What some effects need besides colors: a meter's source (Design::MeterSource).
    Q_INVOKABLE void setLayerOption(int index, int option);
    // which: 0 for the first color, 1 for the second.
    Q_INVOKABLE void setLayerColor(int index, int which, const QColor &color);
    Q_INVOKABLE void setLayerRandom(int index, bool on);
    // Adds the keys to the layer, or takes them out of it.
    Q_INVOKABLE void setLayerKeys(int index, const QList<int> &codes, bool add);
    // The custom lighting at a moment, key code (as text) to color, for the
    // preview; the same frame the service sends.
    Q_INVOKABLE QVariantMap frameAt(qint64 ms) const;

    bool oledOn() const { return m_oledOn; }
    int oledBrightness() const { return m_oledBrightness; }
    int oledPreset() const { return m_oledPreset; }
    QString oledFile() const { return m_oledFile; }
    int oledFrames() const { return m_previewFrames.size(); }
    int previewRevision() const { return m_previewRevision; }
    QImage previewFrame(int index) const;

    void setOledOn(bool on);

    // The OLED clock needs the time pushed every minute, so turning it on
    // starts omaglow-clock.service and anything else on the display stops it.
    using ServiceRunner = std::function<int(const QStringList &args, QString *error)>;
    bool oledClock() const { return m_oledClock; }
    void setOledClock(bool on);
    void setServiceRunner(ServiceRunner runner) { m_serviceRunner = std::move(runner); }

    // Spectrum bars of what is playing, kept up by omaglow-music.service.
    bool oledMusic() const { return m_oledMusic; }
    void setOledMusic(bool on);
    int musicStyle() const { return m_musicStyle; }
    void setMusicStyle(int style);
    // Reads whether the service is really enabled, in case it was changed
    // outside the app.
    void syncClockService();

    // Reads whether the lighting service is enabled, in case it changed
    // outside the app.
    void syncLightsService();

    QVariantList looks() const;
    // Saves the current lighting under the name, replacing a look with the
    // same name.
    Q_INVOKABLE void saveLook(const QString &name);
    QVariantMap mouseZones() const;
    // Sets one mouse zone, sends it when the mouse is plugged in, and keeps it.
    Q_INVOKABLE void setMouseZone(int led, int effect, const QColor &color1, const QColor &color2, bool twoColors,
                                  int brightness);
    // Opens the mouse, or returns nullptr when it isn't there; for tests.
    using MouseOpener = std::function<Razer::FeaturePort *()>;
    void setMouseOpener(MouseOpener opener) { m_mouseOpener = std::move(opener); }
    void setMouseFinder(Finder finder) { m_mouseFinder = std::move(finder); }
    QVariantMap laptopAura() const;
    bool laptopAvailable() const;
    // Sets the laptop keyboard and keeps it; looks saved afterwards include it.
    Q_INVOKABLE void setLaptopAura(int effect, const QColor &color1, const QColor &color2, int speed, int direction,
                                   int brightness);
    // Runs asusctl with these arguments and returns its exit code; for tests.
    bool followTheme() const { return m_followTheme; }
    // Also installs Omarchy's theme-set hook, which runs `omaglow theme-sync`
    // when the app is closed, and removes it when turned off.
    void setFollowTheme(bool on);
    // Static in this color on every device; nothing happens if it was the last applied.
    Q_INVOKABLE void applyThemeColor(const QColor &color);
    void setHooksDirectory(const QString &path) { m_hooksDirectory = path; }
    using AsusctlRunner = std::function<int(const QStringList &args)>;
    void setAsusctlRunner(AsusctlRunner runner) { m_asusctl = std::move(runner); }
    int mouseStatus() const { return m_mouseStatus; }
    // Looks for the mouse again; the poll does this every couple of seconds.
    void refreshMouse();
    Q_INVOKABLE void applyLook(int index);
    Q_INVOKABLE void deleteLook(int index);
    void setOledBrightness(int value);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void applyLighting();
    Q_INVOKABLE void chooseOledPreset(int index);
    Q_INVOKABLE void openOledImage();
    Q_INVOKABLE void uploadOled(const QString &path);
    Q_INVOKABLE void clearError();

    // Every key with its place and what it does now.
    QVariantList keys() const;
    // Everything a key or knob slot can be set to: special actions, then keys.
    QVariantList keyChoices() const;
    int remappedCount() const;
    Keymap::Action keyAction(int key) const { return m_remaps.value(key); }
    Q_INVOKABLE void remap(int key, int code, int param);
    Q_INVOKABLE void resetKeys();

    QVariantList knobFunctions() const;
    QVariantList knobSlots() const;
    bool knobEnabled(int function) const { return m_knob.value(function); }
    Keymap::Action knobSlot(int slot) const { return m_knobSlots.value(slot); }
    Q_INVOKABLE void setKnobFunction(int function, bool enabled);
    Q_INVOKABLE void setKnobSlot(int slot, int code, int param);

    // The keyboard can't say which profile is active or what it holds, so the
    // app remembers each profile's lighting, keys and knob separately.
    int profile() const { return m_profile; }
    void setProfile(int profile);

    int sleepTimeout() const { return m_sleepTimeout; }
    int batteryAlert() const { return m_batteryAlert; }
    int powerSaving() const { return m_powerSaving; }
    int debounceMs() const { return m_debounceMs; }
    void setSleepTimeout(int index);
    void setBatteryAlert(int percent);
    void setPowerSaving(int mode);
    void setDebounceMs(int milliseconds);

    // Sends packets now instead of on the debounce timer; used by tests and
    // before quitting.
    Q_INVOKABLE void flush();

signals:
    void connectionChanged();
    void errorChanged();
    void busyChanged();
    void lightingChanged();
    void oledChanged();
    void keysChanged();
    void knobChanged();
    void powerChanged();
    void profileChanged();
    void looksChanged();
    void mouseChanged();
    void laptopChanged();
    void followThemeChanged();

private:
    void connectDevice();
    void disconnectDevice();
    bool send(const QList<QByteArray> &packets);
    void setError(const QString &message);
    void scheduleLighting();
    QList<QByteArray> designPackets() const;
    void resendDesign();
    void load();
    void store();
    void loadProfile();
    void storeProfile();
    QString profilePrefix() const;

    Finder m_finder;
    Opener m_opener;
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings;
    FilePicker *m_picker = nullptr;
    std::unique_ptr<Link> m_link;
    DeviceNode m_node;
    int m_connection = Disconnected;
    QString m_firmware;
    int m_battery = -1;
    bool m_asleep = false;
    QString m_error;
    bool m_busy = false;
    double m_progress = 0;

    Protocol::Lighting m_lighting;
    bool m_lightingOn = true;
    bool m_custom = false;
    QHash<int, QColor> m_design;
    QList<Design::Layer> m_layers;
    Design::ProfileLighting current() const;
    void setCurrent(const Design::ProfileLighting &lighting);
    bool editLayer(int index);
    bool m_lightingDirty = false;
    QTimer m_lightingTimer;
    QTimer m_pollTimer;
    int m_pollTicks = 0;

    bool m_oledOn = true;
    int m_oledBrightness = 100;
    int m_oledPreset = -1;
    QString m_oledFile;
    QList<QByteArray> m_previewFrames;
    int m_previewRevision = 0;
    QFutureWatcher<bool> m_upload;

    QHash<int, Keymap::Action> m_remaps;      // only keys that aren't default
    QHash<int, bool> m_knob;                  // carousel function -> enabled
    QHash<int, Keymap::Action> m_knobSlots;   // customize slot -> action

    // Armoury's defaults: 3 minutes, alert at 20%, lights off when saving, 15 ms.
    int m_profile = Protocol::DefaultProfile;
    bool m_serviceOn = false; // omaglow-lights.service enabled
    // Starts or stops the service for what is wanted now; false if it couldn't.
    bool updateLightsService();
    QList<Design::Look> m_looks;
    void storeLooks();
    int m_sleepTimeout = 2;
    int m_batteryAlert = 20;
    int m_powerSaving = 1;
    int m_debounceMs = 15;
    // Sends one setting and keeps it only if the keyboard took it.
    void setPowerValue(int &field, int value, const QByteArray &packet);

    bool m_oledClock = false;
    ServiceRunner m_serviceRunner;
    MouseOpener m_mouseOpener;
    int m_stuckPolls = 0;
    void markNotResponding();
    Finder m_mouseFinder;
    AsusctlRunner m_asusctl;
    bool m_followTheme = false;
    QString m_hooksDirectory;
    bool m_hasLaptop = false; // set once the laptop keyboard was picked here
    Laptop::Aura m_laptop;
    bool applyLaptop(const Laptop::Aura &aura);
    int m_mouseStatus = DeviceNode::NotFound;
    Design::MouseZones m_mouse;
    bool applyMouse(const Design::MouseZones &zones);
    bool switchClockService(bool on);
    bool switchMusicService(bool on);
    // Stops whichever live display service is running; false if it couldn't.
    bool stopLiveDisplays();
    bool m_oledMusic = false;
    int m_musicStyle = 1;
    bool runService(const QStringList &args, const QString &failure);
};
