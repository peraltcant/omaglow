#pragma once

#include <QList>
#include <QString>

// The Azoth's keys, numbered as IBM PC/AT key positions (the numbers the
// keyboard uses both for the key being remapped and for a key it should
// send), with their place on the ANSI 75% layout in key units.
namespace Keymap {

struct Key {
    int code;
    const char *label;
    double x, y, width;
};

const QList<Key> &layout();
const Key *find(int code);

// The key's LED in direct mode (column*8+row on the keyboard's matrix), or -1.
int ledOf(int code);
// The key a USB keyboard usage (page 7) reports, -1 for one the Azoth hasn't.
int codeOfUsage(int usage);

// Every key position, in the order Armoury resets them.
QList<int> resetCodes();

// What a key or a knob slot can do: a code and a parameter byte.
struct Action {
    int code = 0xff;
    int param = 0x00;
    bool operator==(const Action &o) const { return code == o.code && param == o.param; }
    bool isDefault() const { return code == 0xff && param == 0x00; }
};

enum Kind { Default, KeyPress, Media, Mouse, Copilot };

struct Choice {
    Kind kind;
    Action action;
    const char *label;
};

// The non-key actions, in the order Armoury lists them.
const QList<Choice> &specialChoices();

Kind kindOf(const Action &action);
QString labelOf(const Action &action); // English; the UI translates it

} // namespace Keymap
