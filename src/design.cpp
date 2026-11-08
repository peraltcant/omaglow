#include "design.h"

#include <QSet>
#include <algorithm>
#include <QtMath>

#include "keymap.h"
#include "protocol.h"

namespace Design {

namespace {

// One cycle of an effect at each speed.
constexpr qint64 periodMs[] = {1500, 3000, 6000};
// How many key units long the comet's tail is.
constexpr double cometTail = 3.0;
// How many keys the current's tail covers.
constexpr int currentTail = 6;

quint32 mixBits(quint32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

quint32 hashOf(int a, qint64 b) {
    return mixBits(quint32(a) * 0x9e3779b9U ^ mixBits(quint32(b)));
}

QColor scaled(const QColor &c, double f) {
    f = qBound(0.0, f, 1.0);
    return QColor(qRound(c.red() * f), qRound(c.green() * f), qRound(c.blue() * f));
}

QColor mixed(const QColor &a, const QColor &b, double f) {
    f = qBound(0.0, f, 1.0);
    return QColor(qRound(a.red() + (b.red() - a.red()) * f), qRound(a.green() + (b.green() - a.green()) * f),
                  qRound(a.blue() + (b.blue() - a.blue()) * f));
}

QColor hue(double degrees) {
    const int h = int(std::floor(degrees)) % 360;
    return QColor::fromHsv(h < 0 ? h + 360 : h, 255, 255).toRgb();
}

double fraction(double v) {
    return v - std::floor(v);
}

// The color for cycle n: random ones per cycle (and per key when salt is a
// key), or the two colors taking turns.
QColor pick(const Layer &layer, int salt, qint64 n) {
    if (layer.randomColors)
        return hue(hashOf(salt, n) % 360);
    return n % 2 ? layer.color2 : layer.color1;
}

double centerOf(int code) {
    const Keymap::Key *key = Keymap::find(code);
    return key ? key->x + key->width / 2 : 0;
}

void drawLayer(const Layer &layer, qint64 ms, Keys &out) {
    if (layer.keys.isEmpty())
        return;
    const qint64 period = periodMs[qBound(0, layer.speed, 2)];
    const double t = double(ms % period) / period;
    const qint64 n = ms / period;
    // The current's path: row by row, left to right then back (a zigzag).
    QList<int> path;
    if (layer.effect == Current) {
        path = layer.keys;
        std::sort(path.begin(), path.end(), [](int a, int b) {
            const Keymap::Key *ka = Keymap::find(a), *kb = Keymap::find(b);
            if (ka->y != kb->y)
                return ka->y < kb->y;
            return int(ka->y) % 2 ? ka->x > kb->x : ka->x < kb->x;
        });
    }
    double minX = 1e9, maxX = -1e9;
    for (int code : layer.keys) {
        minX = qMin(minX, centerOf(code));
        maxX = qMax(maxX, centerOf(code));
    }
    for (int code : layer.keys) {
        const double x = centerOf(code);
        QColor c;
        switch (layer.effect) {
        case Breathing:
            c = scaled(pick(layer, 0, n), 0.5 - 0.5 * std::cos(2 * M_PI * t));
            break;
        case Strobing: {
            const qint64 flash = period / 3;
            c = ms % flash < flash * 3 / 10 ? pick(layer, 0, ms / flash) : QColor(0, 0, 0);
            break;
        }
        case ColorCycle:
            c = hue(t * 360);
            break;
        case Rainbow:
            c = hue(fraction(x / 16.0 - t) * 360);
            break;
        case StarryNight: {
            // Each key twinkles on its own clock, and only in some cycles.
            const quint32 h = hashOf(code, 0);
            const qint64 own = period * (75 + h % 50) / 100;
            const qint64 local = ms + h % own;
            const qint64 cycle = local / own;
            const double phase = double(local % own) / own;
            const bool twinkles = hashOf(code, cycle) % 2 == 0 && phase < 0.4;
            c = twinkles ? scaled(pick(layer, code, cycle), std::sin(M_PI * phase / 0.4)) : QColor(0, 0, 0);
            break;
        }
        case Comet: {
            const double head = minX - cometTail + (maxX - minX + 2 * cometTail) * t;
            const double behind = head - x;
            c = behind >= 0 && behind < cometTail ? scaled(pick(layer, 0, n), 1 - behind / cometTail)
                                                  : QColor(0, 0, 0);
            break;
        }
        case Current: {
            // The head goes round the path once per cycle, the tail fading.
            const double head = t * (path.size() + currentTail);
            const double behind = head - path.indexOf(code);
            c = behind >= 0 && behind < currentTail ? scaled(pick(layer, 0, n), 1 - behind / currentTail)
                                                     : QColor(0, 0, 0);
            break;
        }
        case Tide:
            c = mixed(layer.color2, layer.color1, 0.5 + 0.5 * std::sin(2 * M_PI * (x / 6.0 - t)));
            break;
        default:
            c = layer.color1;
        }
        out.insert(code, c);
    }
}

} // namespace

Keys render(const ProfileLighting &lighting, qint64 ms) {
    Keys out = lighting.keys;
    for (const Layer &layer : lighting.layers)
        drawLayer(layer, qMax<qint64>(0, ms), out);
    return out;
}

bool animated(const ProfileLighting &lighting) {
    if (!lighting.on || !lighting.custom)
        return false;
    for (const Layer &layer : lighting.layers)
        if (layer.effect != StaticLayer && !layer.keys.isEmpty())
            return true;
    return false;
}

QList<Layer> decodeLayers(const QStringList &entries) {
    QList<Layer> layers;
    for (const QString &entry : entries) {
        Layer layer;
        layer.effect = qBound(0, entry.section('|', 0, 0).toInt(), LayerEffectCount - 1);
        layer.speed = qBound(0, entry.section('|', 1, 1).toInt(), 2);
        if (const QColor c(entry.section('|', 2, 2)); c.isValid())
            layer.color1 = c;
        if (const QColor c(entry.section('|', 3, 3)); c.isValid())
            layer.color2 = c;
        layer.randomColors = entry.section('|', 4, 4) == "1";
        QSet<int> seen;
        for (const QString &code : entry.section('|', 5, 5).split(' ', Qt::SkipEmptyParts)) {
            const int value = code.toInt();
            if (Keymap::find(value) && !seen.contains(value)) {
                seen.insert(value);
                layer.keys.append(value);
            }
        }
        layer.name = entry.section('|', 6);
        layers.append(layer);
    }
    return layers;
}

QStringList encodeLayers(const QList<Layer> &layers) {
    QStringList entries;
    for (const Layer &layer : layers) {
        QStringList codes;
        for (int code : layer.keys)
            codes.append(QString::number(code));
        entries.append(QString("%1|%2|%3|%4|%5|%6|%7")
                           .arg(layer.effect)
                           .arg(layer.speed)
                           .arg(layer.color1.name(), layer.color2.name(), layer.randomColors ? "1" : "0",
                                codes.join(' '), layer.name));
    }
    return entries;
}

QString profilePrefix(int profile) {
    return profile == Protocol::DefaultProfile ? QString() : QString("profile%1/").arg(profile);
}

Keys decode(const QString &text) {
    Keys keys;
    for (const QString &entry : text.split(',', Qt::SkipEmptyParts)) {
        const QStringList parts = entry.split(':');
        bool ok = false;
        const int code = parts.value(0).toInt(&ok);
        const QColor color(parts.value(1));
        if (ok && Keymap::find(code) && color.isValid())
            keys.insert(code, color);
    }
    return keys;
}

QString encode(const Keys &keys) {
    QStringList entries;
    for (const auto &k : Keymap::layout())
        if (keys.contains(k.code))
            entries.append(QString("%1:%2").arg(k.code).arg(keys.value(k.code).name()));
    return entries.join(',');
}

// The keyboard only takes a frame the way Armoury sends it: every LED of its
// 16 by 6 matrix, row by row, the ones without a key dark.
constexpr int matrixColumns = 16;
constexpr int matrixRows = 6;

// LEDs are linear while colors are picked the way a screen shows them, so
// each channel goes through the screen's gamma curve; otherwise dark shades
// look almost as bright as full ones and waves in one hue don't show.
static int toLed(int value, int brightness) {
    static const QList<int> table = [] {
        QList<int> t;
        for (int v = 0; v < 256; ++v)
            t.append(qRound(255 * std::pow(v / 255.0, 2.2)));
        return t;
    }();
    return table[qBound(0, value, 255)] * brightness / 100;
}

QList<QByteArray> packets(const Keys &keys, int brightness) {
    const int b = qBound(0, brightness, 100);
    QHash<int, QColor> byLed;
    for (const auto &k : Keymap::layout()) {
        const QColor c = keys.value(k.code, QColor(0, 0, 0));
        byLed.insert(Keymap::ledOf(k.code), QColor(toLed(c.red(), b), toLed(c.green(), b), toLed(c.blue(), b)));
    }
    QList<QPair<int, QColor>> leds;
    for (int row = 0; row < matrixRows; ++row)
        for (int column = 0; column < matrixColumns; ++column) {
            const int led = column * 8 + row;
            leds.append({led, byLed.value(led, QColor(0, 0, 0))});
        }
    return Protocol::directFrame(leds);
}

ProfileLighting load(QSettings &s, int profile) {
    return loadAt(s, profilePrefix(profile));
}

ProfileLighting loadAt(QSettings &s, const QString &p) {
    ProfileLighting out;
    out.on = s.value(p + "lighting/on", true).toBool();
    out.custom = s.value(p + "lighting/custom", false).toBool();
    Protocol::Lighting &l = out.lighting;
    l.effect = qBound(0, s.value(p + "lighting/effect", 0).toInt(), Protocol::EffectCount - 1);
    l.brightness = qBound(0, s.value(p + "lighting/brightness", 100).toInt(), 100);
    l.speed = qBound(0, s.value(p + "lighting/speed", 1).toInt(), 2);
    l.colorMode = qBound(0, s.value(p + "lighting/colorMode", 0).toInt(), 2);
    l.color1 = QColor(s.value(p + "lighting/color1", "#ffffff").toString());
    l.color2 = QColor(s.value(p + "lighting/color2", "#ff00ff").toString());
    l.direction = s.value(p + "lighting/direction", -1).toInt();
    l.thickness = qBound(0, s.value(p + "lighting/thickness", 2).toInt(), 2);
    out.keys = decode(s.value(p + "lighting/design").toString());
    out.layers = decodeLayers(s.value(p + "lighting/layers").toStringList());
    return out;
}

void storeAt(QSettings &s, const QString &p, const ProfileLighting &in) {
    const Protocol::Lighting &l = in.lighting;
    s.setValue(p + "lighting/on", in.on);
    s.setValue(p + "lighting/effect", l.effect);
    s.setValue(p + "lighting/brightness", l.brightness);
    s.setValue(p + "lighting/speed", l.speed);
    s.setValue(p + "lighting/colorMode", l.colorMode);
    s.setValue(p + "lighting/color1", l.color1.name());
    s.setValue(p + "lighting/color2", l.color2.name());
    s.setValue(p + "lighting/direction", l.direction);
    s.setValue(p + "lighting/thickness", l.thickness);
    s.setValue(p + "lighting/custom", in.custom);
    s.setValue(p + "lighting/design", encode(in.keys));
    if (in.layers.isEmpty())
        s.remove(p + "lighting/layers");
    else
        s.setValue(p + "lighting/layers", encodeLayers(in.layers));
}

namespace {
const QList<QPair<int, QString>> mouseKeys{{Razer::Logo, "logo"}, {Razer::ScrollWheel, "wheel"}};
}

MouseZones loadMouse(QSettings &s, const QString &prefix) {
    MouseZones zones;
    for (const auto &key : mouseKeys) {
        bool ok = false;
        const Razer::Zone zone = Razer::decodeZone(s.value(prefix + "mouse/" + key.second).toString(), &ok);
        if (ok)
            zones.insert(key.first, zone);
    }
    return zones;
}

void storeMouse(QSettings &s, const QString &prefix, const MouseZones &zones) {
    for (const auto &key : mouseKeys) {
        if (zones.contains(key.first))
            s.setValue(prefix + "mouse/" + key.second, Razer::encodeZone(zones.value(key.first)));
        else
            s.remove(prefix + "mouse/" + key.second);
    }
}

QList<Look> loadLooks(QSettings &s) {
    QList<Look> looks;
    const int count = s.beginReadArray("looks");
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        const QString name = s.value("name").toString();
        if (!name.isEmpty()) {
            Look look{name, loadAt(s, QString()), loadMouse(s, QString())};
            look.laptop = Laptop::decode(s.value("laptop/aura").toString(), &look.hasLaptop);
            looks.append(look);
        }
    }
    s.endArray();
    return looks;
}

void storeLooks(QSettings &s, const QList<Look> &looks) {
    s.remove("looks");
    s.beginWriteArray("looks", looks.size());
    for (int i = 0; i < looks.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", looks[i].name);
        storeAt(s, QString(), looks[i].lighting);
        storeMouse(s, QString(), looks[i].mouse);
        if (looks[i].hasLaptop)
            s.setValue("laptop/aura", Laptop::encode(looks[i].laptop));
    }
    s.endArray();
}

namespace {
// Settings store #rrggbb, so colors are compared as the keyboard gets them.
bool sameKeys(const Keys &a, const Keys &b) {
    if (a.size() != b.size())
        return false;
    for (auto it = a.begin(); it != a.end(); ++it)
        if (!b.contains(it.key()) || b.value(it.key()).rgb() != it.value().rgb())
            return false;
    return true;
}
}

bool sameLook(const ProfileLighting &a, const ProfileLighting &b) {
    if (a.on != b.on)
        return false;
    if (!a.on)
        return true;
    if (a.custom != b.custom || a.lighting.brightness != b.lighting.brightness)
        return false;
    if (a.custom)
        return sameKeys(a.keys, b.keys) && encodeLayers(a.layers) == encodeLayers(b.layers);
    const Protocol::Lighting &x = a.lighting, &y = b.lighting;
    return x.effect == y.effect && x.speed == y.speed && x.colorMode == y.colorMode && x.color1.rgb() == y.color1.rgb()
        && x.color2.rgb() == y.color2.rgb() && x.direction == y.direction && x.thickness == y.thickness;
}

int findLook(const QList<Look> &looks, const QString &name) {
    for (int i = 0; i < looks.size(); ++i)
        if (looks[i].name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

QList<QByteArray> packetsFor(const ProfileLighting &profile) {
    if (!profile.on)
        return {Protocol::lightingOff()};
    if (profile.custom)
        return packets(render(profile, 0), profile.lighting.brightness);
    return Protocol::lighting(profile.lighting);
}

QList<QByteArray> packetsForProfile(QSettings &settings, int profile) {
    return packetsFor(load(settings, profile));
}

} // namespace Design
