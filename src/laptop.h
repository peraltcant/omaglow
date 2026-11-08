#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>

// The laptop's keyboard lighting (ROG Zephyrus G16 and other Aura laptops)
// through asusctl, which talks to asusd. Only the effects asusd lists as
// supported on the G16 with a clear meaning are offered.
namespace Laptop {

enum Effect { Static, Breathe, RainbowCycle, RainbowWave, EffectCount };

struct Aura {
    int effect = Static;
    QColor color1 = QColor(255, 0, 0);
    QColor color2 = QColor(0, 0, 255);
    int speed = 1;      // 0 low, 1 med, 2 high
    int direction = 3;  // 0 up, 1 down, 2 left, 3 right
    int brightness = 2; // 0 off, 1 low, 2 med, 3 high
};

// The asusctl argument lists that set it: the effect, then the brightness.
// Off sends only the brightness, so the effect is there when it comes back.
QList<QStringList> commands(const Aura &aura);

// Stored as "effect|#rrggbb|#rrggbb|speed|direction|brightness".
QString encode(const Aura &aura);
Aura decode(const QString &text, bool *ok = nullptr);

} // namespace Laptop
