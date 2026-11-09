#include "design.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
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

// Light added over a color, mixed as light mixes (in linear light, gamma
// 2.2 as the keyboard is driven): half gold over blue is a pale bright color,
// where mixing the numbers would give a dim grey.
QColor lit(const QColor &under, const QColor &light, double f) {
    f = qBound(0.0, f, 1.0);
    auto channel = [f](int a, int b) {
        const double la = std::pow(a / 255.0, 2.2), lb = std::pow(b / 255.0, 2.2);
        return qRound(255 * std::pow(la + (lb - la) * f, 1 / 2.2));
    };
    return QColor(channel(under.red(), light.red()), channel(under.green(), light.green()),
                  channel(under.blue(), light.blue()));
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

// Smooth noise along t for one seed, 0-1: random values at whole steps,
// eased in between, so flames flicker without jumping.
double noise(int seed, double t) {
    const qint64 step = qint64(std::floor(t));
    const double a = (hashOf(seed, step) % 1000) / 999.0;
    const double b = (hashOf(seed, step + 1) % 1000) / 999.0;
    const double f = t - step;
    return a + (b - a) * f * f * (3 - 2 * f);
}

// One bump of a pulse that starts at start and lasts width, 0-1, else 0.
double pulse(double t, double start, double width) {
    return t >= start && t < start + width ? std::sin(M_PI * (t - start) / width) : 0;
}

double centerOf(int code) {
    const Keymap::Key *key = Keymap::find(code);
    return key ? key->x + key->width / 2 : 0;
}

void drawLayer(const Layer &layer, qint64 ms, const Live &live, Keys &out) {
    // Only keys the keyboard has; a hand-edited layer could name others.
    QList<int> keys;
    for (int code : layer.keys)
        if (Keymap::find(code))
            keys.append(code);
    if (keys.isEmpty())
        return;
    const qint64 period = periodMs[qBound(0, layer.speed, 2)];
    const double t = double(ms % period) / period;
    const qint64 n = ms / period;
    // The current's path: row by row, left to right then back (a zigzag).
    QList<int> path;
    if (layer.effect == Current) {
        path = keys;
        std::sort(path.begin(), path.end(), [](int a, int b) {
            const Keymap::Key *ka = Keymap::find(a), *kb = Keymap::find(b);
            if (ka->y != kb->y)
                return ka->y < kb->y;
            return int(ka->y) % 2 ? ka->x > kb->x : ka->x < kb->x;
        });
    }
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (int code : keys) {
        minX = qMin(minX, centerOf(code));
        maxX = qMax(maxX, centerOf(code));
        const double y = Keymap::find(code)->y;
        minY = qMin(minY, y);
        maxY = qMax(maxY, y);
    }
    // Where a key sits in the layer, 0-1 from left to right and bottom to top.
    auto across = [&](double x) { return maxX > minX ? (x - minX) / (maxX - minX) : 0.0; };
    auto up = [&](int code) { return maxY > minY ? (maxY - Keymap::find(code)->y) / (maxY - minY) : 0.0; };
    // Time in cycles, for the effects that move smoothly instead of looping.
    const double cycles = double(ms) / period;
    for (int code : keys) {
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
        case Fire: {
            // Heat falls off toward the top; each column flickers on its own.
            const double flicker = noise(int(std::floor(x)), cycles * 4) - 0.5;
            const double heat = qBound(0.0, 1.0 - up(code) * 1.15 + flicker * 0.7, 1.0);
            c = scaled(mixed(layer.color1, layer.color2, heat), qMin(1.0, heat * 1.6));
            break;
        }
        case Plasma: {
            const double y = Keymap::find(code)->y, p = 2 * M_PI * cycles / 2;
            const double v = std::sin(x / 4.0 + p) + std::sin(y / 1.6 - p * 0.7) + std::sin((x + y * 2) / 5.0 + p * 1.3);
            const double f = (v + 3) / 6;
            c = layer.randomColors ? hue(f * 360 + cycles * 60)
                                   : mixed(layer.color1, layer.color2, 0.5 - 0.5 * std::cos(2 * M_PI * f));
            break;
        }
        case Scanner: {
            // Left to right and back once per cycle, with a short fading tail.
            const double head = minX + (maxX - minX) * (t < 0.5 ? t * 2 : 2 - t * 2);
            const double d = std::abs(x - head);
            c = d < 1.5 ? scaled(pick(layer, 0, n), 1 - d / 1.5) : QColor(0, 0, 0);
            break;
        }
        case Fireflies: {
            // Slow glows, each key on its own longer clock, a third of them at a time.
            const quint32 h = hashOf(code, 1);
            const qint64 own = period * (200 + h % 200) / 100;
            const qint64 local = ms + h % own;
            const qint64 cycle = local / own;
            const double phase = double(local % own) / own;
            const bool glows = hashOf(code, cycle) % 3 == 0 && phase < 0.6;
            const double s = std::sin(M_PI * phase / 0.6);
            c = glows ? scaled(pick(layer, code, cycle), s * s) : QColor(0, 0, 0);
            break;
        }
        case Heartbeat: {
            // Lub-dub: a full beat, a softer one, then a dim rest.
            const double beat = qMax(pulse(t, 0.0, 0.12), 0.6 * pulse(t, 0.2, 0.12));
            c = scaled(pick(layer, 0, n), qMax(0.08, beat));
            break;
        }
        case Gradient:
            c = mixed(layer.color1, layer.color2, across(x));
            break;
        case Music: {
            // A bar per column, its band by position; lit up to the band's level.
            if (live.spectrum.isEmpty()) {
                c = QColor(0, 0, 0);
                break;
            }
            const int band = qRound(across(x) * (live.spectrum.size() - 1));
            const double level = live.spectrum.at(band) / 255.0;
            const double rows = maxY - minY + 1;
            const double threshold = (up(code) * (rows - 1) + 0.5) / rows;
            c = level >= threshold ? mixed(layer.color1, layer.color2, up(code)) : QColor(0, 0, 0);
            break;
        }
        case Workspaces: {
            // Number keys only: 1-9, and 0 for the tenth. Omarchy gives each
            // monitor a block of ten workspaces (11-20 for the second), and
            // Super + 1-0 goes to the focused monitor's; the keys follow it too.
            bool number = false;
            int workspace = QString::fromUtf8(Keymap::find(code)->label).toInt(&number);
            if (!number)
                continue;
            if (workspace == 0)
                workspace = 10;
            if (live.activeWorkspace > 0)
                workspace += (live.activeWorkspace - 1) / 10 * 10;
            c = workspace == live.activeWorkspace         ? layer.color2
                : live.busyWorkspaces.contains(workspace) ? layer.color1
                                                          : QColor(0, 0, 0);
            break;
        }
        case Meter: {
            // A bar from the left, all of it in one color between the first
            // (fine) and the second (busy, hot, or a battery running out).
            double value = -1, danger = 0;
            if (layer.option == TemperatureMeter && live.temperature >= 0)
                value = danger = qBound(0.0, (live.temperature - 40) / 55, 1.0);
            else if (layer.option == BatteryMeter && live.battery >= 0)
                danger = 1 - (value = qBound(0.0, live.battery / 100, 1.0));
            else if (layer.option == CpuMeter && live.cpu >= 0)
                value = danger = qBound(0.0, live.cpu, 1.0);
            c = value > 0 && across(x) <= value + 1e-9 ? mixed(layer.color1, layer.color2, danger) : QColor(0, 0, 0);
            break;
        }
        case Timer: {
            // Ready in the first color; while running the keys go dark from
            // the right; when done they blink in the second color for a minute.
            const qint64 duration = qint64(layer.option > 0 ? layer.option : DefaultTimerMinutes) * 60000;
            const qint64 elapsed = live.now - live.timerStartedAt;
            if (live.timerStartedAt <= 0 || elapsed < 0 || elapsed >= duration + TimerDoneMs)
                c = layer.color1;
            else if (elapsed >= duration)
                c = (ms / 500) % 2 ? QColor(0, 0, 0) : layer.color2;
            else {
                // Each key holds its share of the time, the rightmost going first.
                int rank = 0;
                for (int other : keys)
                    rank += centerOf(other) < x;
                c = 1 - double(elapsed) / duration > double(rank) / keys.size() ? layer.color1 : QColor(0, 0, 0);
            }
            break;
        }
        case Typing: {
            // A ring from each pressed key, as wide as six keys at the end of
            // half a cycle, fading as it goes; the brightest one wins.
            const double life = period / 2.0, y = Keymap::find(code)->y;
            double best = 0;
            QColor color = layer.color1;
            for (auto it = live.pressedAt.begin(); it != live.pressedAt.end(); ++it) {
                const Keymap::Key *pressed = Keymap::find(it.key());
                const double age = ms - it.value();
                if (!pressed || age < 0 || age >= life)
                    continue;
                const double distance = std::hypot(x - centerOf(it.key()), y - pressed->y);
                const double ring = 1 - std::abs(distance - age / life * 6) / 1.2;
                const double glow = (1 - age / life) * qMax(0.0, distance < 0.5 ? qMax(ring, 1.0 - age / life) : ring);
                if (glow > best) {
                    best = glow;
                    color = layer.randomColors ? hue(hashOf(it.key(), it.value()) % 360)
                                               : mixed(layer.color1, layer.color2, qMin(1.0, distance / 6));
                }
            }
            // Keys no ripple reaches show what is under the layer.
            if (best <= 0)
                continue;
            c = scaled(color, best);
            break;
        }
        case Shockwave: {
            // Two beats per cycle, as Heartbeat's, each sending a ring from the
            // bottom middle of the layer (the space bar, on the whole keyboard)
            // to its edges, blended over what is under it.
            // The origin: the bottom row key under the middle of the layer.
            const double middle = (minX + maxX) / 2;
            double ox = middle, nearest = 1e9;
            for (int other : keys) {
                const Keymap::Key *k = Keymap::find(other);
                if (k->y != maxY)
                    continue;
                const double away = middle >= k->x && middle < k->x + k->width ? 0 : std::abs(centerOf(other) - middle);
                if (away < nearest) {
                    nearest = away;
                    ox = centerOf(other);
                }
            }
            const double y = Keymap::find(code)->y;
            const double reach = std::hypot(qMax(ox - minX, maxX - ox), maxY - minY) + 1;
            const double d = std::hypot(x - ox, y - maxY);
            double glow = 0;
            const double starts[] = {0.0, 0.2}, strengths[] = {1.0, 0.7};
            for (int b = 0; b < 2; ++b) {
                // The drum: the whole layer flashes at once and fades fast.
                const double since = t - starts[b];
                if (since >= 0 && since < 0.1)
                    glow = qMax(glow, strengths[b] * 0.6 * (1 - since / 0.1));
                // The wave: a wide ring that keeps its strength most of the way.
                const double p = since / 0.45;
                if (p < 0 || p >= 1)
                    continue;
                const double ring = qMax(0.0, 1 - std::abs(d - p * reach) / 1.8);
                glow = qMax(glow, strengths[b] * ring * std::pow(1 - p, 0.4));
            }
            if (glow <= 0.01)
                continue;
            c = lit(out.value(code, QColor(0, 0, 0)), mixed(layer.color1, layer.color2, d / reach), glow);
            break;
        }
        case Embers: {
            // Sparks born at the bottom drift up, swaying, and fade on the way;
            // each on its own clock, a new column every time it is born.
            const double y = Keymap::find(code)->y;
            const int count = qMax(3, int(keys.size()) / 8);
            double best = 0, rise = 0;
            int spark = 0;
            for (int i = 0; i < count; ++i) {
                const quint32 h = hashOf(i, 7);
                const qint64 life = period * (80 + h % 40) / 100;
                const qint64 local = ms + h % life;
                const qint64 born = local / life;
                const double p = double(local % life) / life;
                const double column = minX + (hashOf(i, born) % 1000) / 999.0 * (maxX - minX);
                const double sx = column + 0.5 * std::sin(2 * M_PI * (p + i * 0.37));
                const double sy = maxY - p * (maxY - minY + 0.5);
                const double near = 1 - std::hypot(x - sx, (y - sy) * 1.3) / 0.9;
                const double glow = near * std::pow(1 - p, 0.7);
                if (glow > best) {
                    best = glow;
                    rise = p;
                    spark = i;
                }
            }
            if (best <= 0.01)
                continue;
            const QColor color = layer.randomColors ? hue(hashOf(spark, 3) % 360) : mixed(layer.color1, layer.color2, rise);
            c = lit(out.value(code, QColor(0, 0, 0)), color, best);
            break;
        }
        case Notifications: {
            // Four blinks over two seconds, the colors taking turns; otherwise
            // the keys show what is under the layer.
            const qint64 since = ms - live.notifiedAt;
            if (live.notifiedAt < 0 || since < 0 || since >= 2000)
                continue;
            c = (since / 250) % 2 ? QColor(0, 0, 0) : (since / 500) % 2 ? layer.color2 : layer.color1;
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

Keys render(const ProfileLighting &lighting, qint64 ms, const Live &live) {
    Keys out = lighting.keys;
    for (const Layer &layer : lighting.layers)
        drawLayer(layer, qMax<qint64>(0, ms), live, out);
    return out;
}

int liveNeeds(const ProfileLighting &lighting) {
    int needs = 0;
    if (!lighting.on || !lighting.custom)
        return needs;
    for (const Layer &layer : lighting.layers) {
        if (layer.keys.isEmpty())
            continue;
        if (layer.effect == Music)
            needs |= NeedsMusic;
        else if (layer.effect == Workspaces)
            needs |= NeedsWorkspaces;
        else if (layer.effect == Meter)
            needs |= NeedsSystem;
        else if (layer.effect == Notifications)
            needs |= NeedsNotifications;
        else if (layer.effect == Timer)
            needs |= NeedsTimer;
        else if (layer.effect == Typing)
            needs |= NeedsTyping;
    }
    return needs;
}

Live demoLive(qint64 ms) {
    Live live;
    const double s = ms / 1000.0;
    for (int band = 0; band < 26; ++band)
        live.spectrum.append(qBound(0, int(255 * (0.55 + 0.45 * std::sin(s * 3 + band * 0.7)) * (1 - band / 40.0)), 255));
    live.busyWorkspaces = {1, 2, 4};
    live.activeWorkspace = 2;
    live.cpu = 0.45 + 0.3 * std::sin(s / 2);
    live.temperature = 60 + 15 * std::sin(s / 3);
    live.battery = 70;
    live.notifiedAt = ms - ms % 5000; // a notification every five seconds
    return live;
}

bool animated(const ProfileLighting &lighting) {
    if (!lighting.on || !lighting.custom)
        return false;
    for (const Layer &layer : lighting.layers)
        if (layer.effect != StaticLayer && layer.effect != Gradient && !layer.keys.isEmpty())
            return true;
    return false;
}

QList<Layer> decodeLayers(const QStringList &entries) {
    QList<Layer> layers;
    for (const QString &entry : entries) {
        Layer layer;
        const QString effect = entry.section('|', 0, 0);
        layer.effect = qBound(0, effect.section(':', 0, 0).toInt(), LayerEffectCount - 1);
        layer.option = qMax(0, effect.section(':', 1, 1).toInt());
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
        const QString effect = layer.option ? QString("%1:%2").arg(layer.effect).arg(layer.option)
                                            : QString::number(layer.effect);
        entries.append(QString("%1|%2|%3|%4|%5|%6|%7")
                           .arg(effect)
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
            look.favorite = s.value("favorite", false).toBool();
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
        if (looks[i].favorite)
            s.setValue("favorite", true);
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

namespace {

// Names in look files. Their order is the enums', and only grows.
const QStringList layerEffectNames{"static", "breathing", "strobing", "color-cycle", "rainbow", "starry-night",
                                   "comet", "tide", "current", "fire", "plasma", "scanner", "fireflies",
                                   "heartbeat", "gradient", "music", "workspaces", "meter", "notifications",
                                   "timer", "typing", "shockwave", "embers"};
const QStringList meterSourceNames{"cpu", "temperature", "battery"};
const QStringList speedNames{"fast", "normal", "slow"};           // keyboard and layers: 0 is fast
const QStringList laptopSpeedNames{"slow", "normal", "fast"};     // asusctl: 0 is low
const QStringList colorModeNames{"single", "double", "random"};
const QStringList thicknessNames{"slim", "normal", "thick"};
const QStringList mouseEffectNames{"off", "static", "breathing", "spectrum"};
const QStringList laptopEffectNames{"static", "breathe", "rainbow-cycle", "rainbow-wave"};
const QStringList directionNames{"up", "down", "left", "right"};
const QStringList laptopBrightnessNames{"off", "low", "medium", "high"};
const QList<QPair<int, QString>> mouseZoneNames{{Razer::Logo, "logo"}, {Razer::ScrollWheel, "scroll-wheel"}};

QString keyboardEffectName(int effect) {
    return QString::fromUtf8(Protocol::effectInfo(effect).name).toLower().replace(' ', '-');
}

int keyboardEffectOf(const QString &name) {
    for (int e = 0; e < Protocol::EffectCount; ++e)
        if (keyboardEffectName(e) == name)
            return e;
    return -1;
}

// A name from the list, or the fallback when it isn't one.
int indexOr(const QStringList &names, const QJsonValue &value, int fallback) {
    const int i = names.indexOf(value.toString());
    return i >= 0 ? i : fallback;
}

QColor colorOr(const QJsonValue &value, const QColor &fallback) {
    const QColor c(value.toString());
    return c.isValid() ? c : fallback;
}

QString colorName(const QColor &c) {
    return c.toRgb().name();
}

QString fileError(const char *text) {
    return QCoreApplication::translate("Looks", text);
}

} // namespace

QJsonObject lookToJson(const Look &look) {
    const ProfileLighting &p = look.lighting;
    const Protocol::Lighting &l = p.lighting;
    QJsonObject design;
    for (auto it = p.keys.begin(); it != p.keys.end(); ++it)
        design.insert(QString::number(it.key()), colorName(it.value()));
    QJsonArray layers;
    for (const Layer &layer : p.layers) {
        QJsonArray keys;
        for (int code : layer.keys)
            keys.append(code);
        QJsonObject o{{"name", layer.name},
                      {"effect", layerEffectNames.value(layer.effect)},
                      {"speed", speedNames.value(qBound(0, layer.speed, 2))},
                      {"color1", colorName(layer.color1)},
                      {"color2", colorName(layer.color2)},
                      {"random", layer.randomColors},
                      {"keys", keys}};
        if (layer.effect == Meter)
            o.insert("source", meterSourceNames.value(qBound(0, layer.option, int(MeterSourceCount) - 1)));
        if (layer.effect == Timer)
            o.insert("minutes", layer.option > 0 ? layer.option : int(DefaultTimerMinutes));
        layers.append(o);
    }
    QJsonObject keyboard{
        {"on", p.on},
        {"custom", p.custom},
        {"brightness", l.brightness},
        {"effect", QJsonObject{{"name", keyboardEffectName(l.effect)},
                               {"speed", speedNames.value(qBound(0, l.speed, 2))},
                               {"colors", colorModeNames.value(qBound(0, l.colorMode, 2))},
                               {"color1", colorName(l.color1)},
                               {"color2", colorName(l.color2)},
                               {"direction", l.direction},
                               {"thickness", thicknessNames.value(qBound(0, l.thickness, 2))}}},
        {"studio", QJsonObject{{"design", design}, {"layers", layers}}}};
    QJsonObject json{{"omaglow", "look"}, {"version", 1}, {"name", look.name}, {"keyboard", keyboard}};
    if (!look.mouse.isEmpty()) {
        QJsonObject mouse;
        for (const auto &[led, name] : mouseZoneNames) {
            if (!look.mouse.contains(led))
                continue;
            const Razer::Zone z = look.mouse.value(led);
            mouse.insert(name, QJsonObject{{"effect", mouseEffectNames.value(qBound(0, z.effect, 3))},
                                           {"color1", colorName(z.color1)},
                                           {"color2", colorName(z.color2)},
                                           {"twoColors", z.twoColors},
                                           {"brightness", z.brightness}});
        }
        json.insert("mouse", mouse);
    }
    if (look.hasLaptop) {
        const Laptop::Aura &a = look.laptop;
        json.insert("laptop", QJsonObject{{"effect", laptopEffectNames.value(qBound(0, a.effect, 3))},
                                          {"color1", colorName(a.color1)},
                                          {"color2", colorName(a.color2)},
                                          {"speed", laptopSpeedNames.value(qBound(0, a.speed, 2))},
                                          {"direction", directionNames.value(qBound(0, a.direction, 3))},
                                          {"brightness", laptopBrightnessNames.value(qBound(0, a.brightness, 3))}});
    }
    return json;
}

std::optional<Look> lookFromJson(const QByteArray &bytes, QString *error) {
    auto fail = [error](const QString &why) -> std::optional<Look> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QJsonParseError parse;
    const QJsonObject json = QJsonDocument::fromJson(bytes, &parse).object();
    if (parse.error != QJsonParseError::NoError || json.value("omaglow").toString() != "look")
        return fail(fileError("This file isn't an Omaglow look."));
    if (json.value("version").toInt() > 1)
        return fail(fileError("This look was saved by a newer Omaglow."));
    Look look;
    look.name = json.value("name").toString().trimmed();
    if (look.name.isEmpty())
        return fail(fileError("The look has no name."));

    // The keyboard: what is missing keeps the defaults, as in the settings.
    const QJsonObject keyboard = json.value("keyboard").toObject();
    ProfileLighting &p = look.lighting;
    p.on = keyboard.value("on").toBool(true);
    p.custom = keyboard.value("custom").toBool(false);
    Protocol::Lighting &l = p.lighting;
    const QJsonValue brightness = keyboard.value("brightness");
    l.brightness = brightness.isDouble() && brightness.toInt() >= 0 && brightness.toInt() <= 100 ? brightness.toInt() : 100;
    const QJsonObject effect = keyboard.value("effect").toObject();
    l.effect = qMax(0, keyboardEffectOf(effect.value("name").toString()));
    l.speed = indexOr(speedNames, effect.value("speed"), 1);
    l.colorMode = indexOr(colorModeNames, effect.value("colors"), 0);
    l.color1 = colorOr(effect.value("color1"), l.color1);
    l.color2 = colorOr(effect.value("color2"), l.color2);
    l.direction = effect.value("direction").toInt(-1);
    l.thickness = indexOr(thicknessNames, effect.value("thickness"), 2);
    const QJsonObject studio = keyboard.value("studio").toObject();
    const QJsonObject design = studio.value("design").toObject();
    for (auto it = design.begin(); it != design.end(); ++it) {
        const int code = it.key().toInt();
        const QColor c(it.value().toString());
        if (Keymap::find(code) && c.isValid())
            p.keys.insert(code, c);
    }
    for (const QJsonValue &v : studio.value("layers").toArray()) {
        const QJsonObject o = v.toObject();
        Layer layer;
        layer.effect = layerEffectNames.indexOf(o.value("effect").toString());
        if (layer.effect < 0)
            continue; // an effect this Omaglow doesn't have
        layer.name = o.value("name").toString().trimmed();
        layer.speed = indexOr(speedNames, o.value("speed"), 1);
        layer.color1 = colorOr(o.value("color1"), layer.color1);
        layer.color2 = colorOr(o.value("color2"), layer.color2);
        layer.randomColors = o.value("random").toBool(false);
        if (layer.effect == Meter)
            layer.option = indexOr(meterSourceNames, o.value("source"), CpuMeter);
        if (layer.effect == Timer) {
            const int minutes = o.value("minutes").toInt(DefaultTimerMinutes);
            layer.option = minutes >= 1 && minutes <= 120 ? minutes : int(DefaultTimerMinutes);
        }
        QSet<int> seen;
        for (const QJsonValue &k : o.value("keys").toArray()) {
            const int code = k.toInt(-1);
            if (Keymap::find(code) && !seen.contains(code)) {
                seen.insert(code);
                layer.keys.append(code);
            }
        }
        p.layers.append(layer);
    }

    const QJsonObject mouse = json.value("mouse").toObject();
    for (const auto &[led, name] : mouseZoneNames) {
        if (!mouse.contains(name))
            continue;
        const QJsonObject o = mouse.value(name).toObject();
        Razer::Zone z;
        z.effect = indexOr(mouseEffectNames, o.value("effect"), Razer::Static);
        z.color1 = colorOr(o.value("color1"), z.color1);
        z.color2 = colorOr(o.value("color2"), z.color2);
        z.twoColors = o.value("twoColors").toBool(false);
        z.brightness = qBound(0, o.value("brightness").toInt(100), 100);
        look.mouse.insert(led, z);
    }
    if (json.contains("laptop")) {
        const QJsonObject o = json.value("laptop").toObject();
        Laptop::Aura &a = look.laptop;
        look.hasLaptop = true;
        a.effect = indexOr(laptopEffectNames, o.value("effect"), Laptop::Static);
        a.color1 = colorOr(o.value("color1"), a.color1);
        a.color2 = colorOr(o.value("color2"), a.color2);
        a.speed = indexOr(laptopSpeedNames, o.value("speed"), 1);
        a.direction = indexOr(directionNames, o.value("direction"), 3);
        a.brightness = indexOr(laptopBrightnessNames, o.value("brightness"), 2);
    }
    return look;
}

} // namespace Design
