#include "laptop.h"

namespace Laptop {

namespace {
const QStringList speeds{"low", "med", "high"};
const QStringList directions{"up", "down", "left", "right"};
const QStringList levels{"off", "low", "med", "high"};

QString hex(const QColor &c) {
    return c.toRgb().name().mid(1);
}
}

QList<QStringList> commands(const Aura &a) {
    const QString speed = speeds.value(qBound(0, a.speed, 2));
    const QStringList brightness{"leds", "set", levels.value(qBound(0, a.brightness, 3))};
    if (a.brightness <= 0)
        return {brightness};
    QStringList effect{"aura", "effect"};
    switch (a.effect) {
    case Breathe:
        effect << "breathe" << "--colour" << hex(a.color1) << "--colour2" << hex(a.color2) << "--speed" << speed;
        break;
    case RainbowCycle:
        effect << "rainbow-cycle" << "--speed" << speed;
        break;
    case RainbowWave:
        effect << "rainbow-wave" << "--direction" << directions.value(qBound(0, a.direction, 3)) << "--speed" << speed;
        break;
    default:
        effect << "static" << "-c" << hex(a.color1);
        break;
    }
    return {effect, brightness};
}

QString encode(const Aura &a) {
    return QString("%1|%2|%3|%4|%5|%6")
        .arg(a.effect)
        .arg(a.color1.name(), a.color2.name())
        .arg(a.speed)
        .arg(a.direction)
        .arg(a.brightness);
}

Aura decode(const QString &text, bool *ok) {
    const QStringList f = text.split('|');
    Aura a;
    bool good = f.size() == 6;
    if (good) {
        bool e, s, d, b;
        a.effect = f[0].toInt(&e);
        a.color1 = QColor(f[1]);
        a.color2 = QColor(f[2]);
        a.speed = f[3].toInt(&s);
        a.direction = f[4].toInt(&d);
        a.brightness = f[5].toInt(&b);
        good = e && s && d && b && a.effect >= 0 && a.effect < EffectCount && a.color1.isValid() && a.color2.isValid()
            && a.speed >= 0 && a.speed <= 2 && a.direction >= 0 && a.direction <= 3 && a.brightness >= 0
            && a.brightness <= 3;
    }
    if (ok)
        *ok = good;
    return good ? a : Aura{};
}

} // namespace Laptop
