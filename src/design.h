#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>

#include "protocol.h"
#include "laptop.h"
#include "razer.h"

// A per-key design: key code to color, drawn in direct mode. Shared by the
// app and the lights service, which both send a profile's design.
namespace Design {

using Keys = QHash<int, QColor>;

// Settings prefix of a profile: the Default profile uses the top level, so
// settings from before profiles carry over; profiles 1-5 use profile<N>/.
QString profilePrefix(int profile);

// "code:#rrggbb" pairs separated by commas, unknown keys dropped.
Keys decode(const QString &text);
QString encode(const Keys &keys);

// The whole keyboard, keys left out dark, colors scaled by brightness 0-100.
QList<QByteArray> packets(const Keys &keys, int brightness);

// What a layer does to its keys, the effects Aura Creator offers. The host
// draws them and streams the frames in direct mode.
enum LayerEffect { StaticLayer, Breathing, Strobing, ColorCycle, Rainbow, StarryNight, Comet, Tide, Current, LayerEffectCount };

// A group of keys with one effect. Later layers cover earlier ones.
struct Layer {
    QString name;
    int effect = StaticLayer;
    int speed = 1; // fast, normal, slow
    QColor color1 = QColor(255, 255, 255);
    QColor color2 = QColor(0, 0, 255);
    bool randomColors = false;
    QList<int> keys; // key codes, in the order they were added
};

// One layer per entry: "effect|speed|#color1|#color2|random|codes|name".
QList<Layer> decodeLayers(const QStringList &entries);
QStringList encodeLayers(const QList<Layer> &layers);

// A profile's lighting as the app stores it. A custom lighting is the painted
// design with the layers drawn over it.
struct ProfileLighting {
    bool on = true;
    bool custom = false;
    Protocol::Lighting lighting;
    Keys keys;
    QList<Layer> layers;
};

// Whether two lightings look the same on the keyboard; what one doesn't
// show (the design behind an effect, say) doesn't count.
bool sameLook(const ProfileLighting &a, const ProfileLighting &b);

// The custom lighting at a moment, in milliseconds from any fixed start.
// The same moment always draws the same frame, so the app's preview and the
// service agree. Brightness is applied when sending, not here.
Keys render(const ProfileLighting &lighting, qint64 ms);
// Whether it changes over time and has to be streamed.
bool animated(const ProfileLighting &lighting);
// How often a streamed frame is sent.
constexpr int FrameMs = 62;
ProfileLighting load(QSettings &settings, int profile);
// The same, under any settings prefix (a profile's, or a look's).
ProfileLighting loadAt(QSettings &settings, const QString &prefix);
void storeAt(QSettings &settings, const QString &prefix, const ProfileLighting &lighting);

// A saved lighting with a name, applied by hand or by a game rule.
// Mouse lighting by LED (Razer::Logo, Razer::ScrollWheel).
using MouseZones = QHash<int, Razer::Zone>;
MouseZones loadMouse(QSettings &settings, const QString &prefix);
void storeMouse(QSettings &settings, const QString &prefix, const MouseZones &zones);

struct Look {
    QString name;
    ProfileLighting lighting;
    MouseZones mouse; // empty: the look leaves the mouse as it is
    bool hasLaptop = false; // false: the look leaves the laptop keyboard as it is
    Laptop::Aura laptop;
};
QList<Look> loadLooks(QSettings &settings);
void storeLooks(QSettings &settings, const QList<Look> &looks);
// Index of the look with this name, ignoring case, or -1.
int findLook(const QList<Look> &looks, const QString &name);

// What to send after switching to the profile: its design, its effect, or
// lights off. The keyboard keeps drawing a direct-mode frame across a profile
// switch, so the effect has to be sent too. Nothing here needs saving.
QList<QByteArray> packetsForProfile(QSettings &settings, int profile);
QList<QByteArray> packetsFor(const ProfileLighting &profile);

} // namespace Design
