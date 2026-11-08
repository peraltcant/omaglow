#pragma once

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QList>
#include <QString>

// Packet builders for the Azoth's vendor HID channel (usage page 0xFF00,
// interface 1). Every report is 64 bytes, zero padded, written without a
// report ID byte. The layouts come from Armoury Crate captures; see
// PROTOCOL.md in the reverse-engineering notes.
namespace Protocol {

constexpr int ReportSize = 64;
constexpr int OledWidth = 208;
constexpr int OledHeight = 64;
constexpr int OledBytesPerRow = OledWidth / 2;
constexpr int OledFrameBytes = OledBytesPerRow * OledHeight;
constexpr int OledChunk = 60;
// Through the ROG Omni receiver a report carries 63 bytes, so Armoury sends
// 59 picture bytes per packet instead of 60.
constexpr int OledChunkReceiver = 59;

enum Effect {
    Static = 0,
    Breathing = 1,
    ColorCycle = 2,
    Reactive = 3,
    Rainbow = 4,
    Ripple = 5,
    StarryNight = 6,
    Quicksand = 7,
    Current = 8,
    Raindrop = 9,
};
constexpr int EffectCount = 10;

enum ColorMode { Single = 0, Double = 1, Random = 2 };

// What an effect lets the user change, as Armoury offers it.
struct EffectInfo {
    Effect effect;
    const char *name;
    bool colors;          // single and double colors
    bool random;          // random colors
    bool speed;           // speed presets that the device actually receives
    unsigned char speeds[3]; // fast, normal, slow
    QList<int> directions;   // empty when the effect has no direction
    int defaultDirection;
    bool thickness;
};

const EffectInfo &effectInfo(int effect);

struct Lighting {
    int effect = Static;
    int brightness = 100;   // 0-100
    int speed = 1;          // 0 fast, 1 normal, 2 slow
    int colorMode = Single;
    QColor color1 = QColor(255, 255, 255);
    QColor color2 = QColor(255, 0, 255);
    int direction = -1;     // -1 picks the effect's default
    int thickness = 2;      // 0 slim, 1 normal, 2 thick
};

QByteArray report(std::initializer_list<int> bytes);
QByteArray report(const QByteArray &prefix);

QByteArray save();
QByteArray queryVersion();
QByteArray queryBattery();

QList<QByteArray> lighting(const Lighting &settings);
QByteArray lightingOff();

// Direct mode (Aura Sync): sets the given LEDs, 15 per report. Each report
// says how many LEDs are left, its own included. The keyboard keeps the last
// frame until an effect is chosen again.
constexpr int DirectLedsPerReport = 15;
QList<QByteArray> directFrame(const QList<QPair<int, QColor>> &leds);
// The keyboard doesn't answer every report of a frame, so they are written
// without waiting for a reply.
bool isDirect(const QByteArray &packet);
// Sent once before streaming, not saved: Armoury picks color cycle first.
// Under a still effect the keyboard draws only the first streamed frame.
QByteArray directStart();

QByteArray oledBrightness(int percent);
QByteArray oledPower(bool on);
QByteArray oledPreset(int index);

// A custom OLED animation: frames of OledFrameBytes each (4-bit gray, two
// pixels per byte, high nibble first) with one delay in milliseconds per
// frame. Returns the start packet, the data packets, and the end packet;
// the caller sends save() after them.
QList<QByteArray> oledUpload(const QList<QByteArray> &frames, const QList<int> &delays, int chunk = OledChunk);

// Key remapping: the key's PC/AT position, then what it sends (a PC/AT
// position, a media code, a mouse button, Copilot, or ff 00 for default).
QByteArray remapKey(int key, int code, int param);

// Control knob carousel functions and the customize slots.
enum KnobFunction { KnobVolume = 0x00, KnobMedia = 0x01, KnobKeyboardBrightness = 0x02,
                    KnobOledBrightness = 0x03, KnobEffect = 0x04, KnobCustom = 0x07 };
enum KnobSlot { KnobScrollUp = 0xa4, KnobClick = 0xa5, KnobScrollDown = 0xa6 };
QByteArray knobFunction(int function, bool enabled);
QByteArray knobCustom(int slot, int code, int param);

// Power: idle time before the keyboard sleeps (0-4 = 1, 2, 3, 5, 10
// minutes; SleepNever), the battery level that triggers the lighting alert,
// and what power saving does (0 off, 1 turn off keyboard and OLED lights,
// 2 dim them).
enum { SleepNever = 0xff };
QByteArray sleepTimeout(int index);
QByteArray batteryAlert(int percent);
QByteArray powerSaving(int mode);

// Key debounce in milliseconds, 15-50 as Armoury offers.
QByteArray debounce(int milliseconds);

// Shows this date and time on the OLED clock. The keyboard keeps no time of
// its own and freezes on the last one sent; the display uses 24 hours.
QByteArray clockTime(const QDateTime &when);

// OLED music display, spectrum only: style 1 bars, 2 mirrored, 3 line.
// Then the bands, about ten times a second, one byte each (0-255).
QByteArray musicSpectrumStart(int style);
QByteArray musicBands(const QByteArray &bands);

// Profiles: 1-5, and the default one (6), each with its own lighting, keys
// and knob. Fn + 1-6 on the keyboard switches them too.
enum { DefaultProfile = 6 };
QByteArray switchProfile(int profile);

// Parsed replies.
// Through the ROG Omni receiver the reply carries the receiver's version first.
QString versionFromReply(const QByteArray &reply, bool receiver = false);
int batteryFromReply(const QByteArray &reply);
bool isRejected(const QByteArray &reply);

} // namespace Protocol
