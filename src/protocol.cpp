#include "protocol.h"

#include <QtGlobal>

namespace Protocol {

namespace {

// Armoury's default seven-stop palette for the gradient effects, as it sends
// it after the settings bytes. Rainbow and ripple share it; quicksand uses
// its own. Custom gradients are left for a later version.
const QByteArray rainbowStopsHead = QByteArray::fromHex("070ef500ff1d0006ff");
const QByteArray rainbowStopsMiddle = QByteArray::fromHex("2b00faff3901ff0048fff60056ff7807");
const QByteArray rainbowStopsTail = QByteArray::fromHex("64ff000d");
const QByteArray quicksandHead = QByteArray::fromHex("ffff f6 00 01 ff 00 00 fa ff");
const QByteArray quicksandTail = QByteArray::fromHex("0006fff500ffff000d");

const EffectInfo effects[EffectCount] = {
    {Static, "Static", true, false, false, {0xff, 0xff, 0xff}, {}, -1, false},
    {Breathing, "Breathing", true, true, true, {0x0f, 0x1e, 0x2a}, {}, -1, false},
    {ColorCycle, "Color cycle", false, false, true, {0x12, 0x1f, 0x3e}, {}, -1, false},
    {Reactive, "Reactive", true, true, false, {0x0f, 0x0f, 0x0f}, {}, -1, false},
    {Rainbow, "Rainbow", false, false, true, {0x23, 0x3e, 0x55}, {0, 1, 2, 3, 4, 5, 6, 7}, 0, true},
    {Ripple, "Ripple", false, false, false, {0x64, 0x64, 0x64}, {}, -1, true},
    {StarryNight, "Starry night", true, true, true, {0x0a, 0x0f, 0x19}, {}, -1, false},
    {Quicksand, "Quicksand", false, false, false, {0x0f, 0x0f, 0x0f}, {6, 2}, 2, false},
    {Current, "Current", true, true, true, {0x07, 0x0a, 0x0c}, {}, -1, false},
    {Raindrop, "Raindrop", true, true, true, {0x0e, 0x14, 0x1b}, {}, -1, false},
};

unsigned char clampByte(int value, int low, int high) {
    return static_cast<unsigned char>(qBound(low, value, high));
}

unsigned char modeByte(const EffectInfo &info, int colorMode) {
    // Ripple and quicksand only exist in Armoury's random mode.
    if (info.effect == Ripple || info.effect == Quicksand)
        return 0x01;
    if (!info.colors && !info.random)
        return 0x00;
    if (colorMode == Random && info.random)
        return 0x01;
    if (colorMode == Double && info.colors && info.effect != Static)
        return 0x10;
    return 0x00;
}

void putColor(QByteArray &packet, int offset, const QColor &color) {
    packet[offset] = static_cast<char>(color.red());
    packet[offset + 1] = static_cast<char>(color.green());
    packet[offset + 2] = static_cast<char>(color.blue());
}

} // namespace

const EffectInfo &effectInfo(int effect) {
    return effects[qBound(0, effect, EffectCount - 1)];
}

QByteArray report(const QByteArray &prefix) {
    QByteArray packet = prefix.left(ReportSize);
    packet.append(QByteArray(ReportSize - packet.size(), '\0'));
    return packet;
}

QByteArray report(std::initializer_list<int> bytes) {
    QByteArray prefix;
    for (int b : bytes)
        prefix.append(static_cast<char>(b & 0xff));
    return report(prefix);
}

QByteArray save() { return report({0x50, 0x55}); }
QByteArray queryVersion() { return report({0x12, 0x00}); }
QByteArray queryBattery() { return report({0x12, 0x01}); }

QList<QByteArray> lighting(const Lighting &s) {
    const EffectInfo &info = effectInfo(s.effect);
    const unsigned char brightness = clampByte(s.brightness, 0, 100);
    const unsigned char speed = info.effect == Static ? 0xff : info.speeds[qBound(0, s.speed, 2)];
    const unsigned char mode = modeByte(info, s.colorMode);
    int direction = info.directions.contains(s.direction) ? s.direction : info.defaultDirection;
    const unsigned char dir = direction < 0 ? 0xff : static_cast<unsigned char>(direction);
    const unsigned char thick = info.thickness ? clampByte(s.thickness, 0, 2) : 0xff;

    if (info.effect == Rainbow || info.effect == Ripple) {
        QByteArray first = report({0x51, 0x2c, info.effect, 0x02, speed, brightness, mode, dir, thick});
        first.replace(9, rainbowStopsHead.size(), rainbowStopsHead);
        QByteArray middle = report({0x51, 0x2c, info.effect, 0x01});
        middle.replace(4, rainbowStopsMiddle.size(), rainbowStopsMiddle);
        QByteArray last = report({0x51, 0x2c, info.effect, 0x00});
        last.replace(4, rainbowStopsTail.size(), rainbowStopsTail);
        return {first, middle, last};
    }

    if (info.effect == Quicksand) {
        QByteArray first = report({0x51, 0x2c, info.effect, 0x01, speed, brightness, mode, dir});
        first.replace(8, quicksandHead.size(), quicksandHead);
        QByteArray last = report({0x51, 0x2c, info.effect, 0x00});
        last.replace(4, quicksandTail.size(), quicksandTail);
        return {first, last};
    }

    QByteArray packet = report({0x51, 0x2c, info.effect, 0x00, speed, brightness, mode, dir, thick});
    if (info.colors) {
        putColor(packet, 9, s.color1);
        // Armoury keeps sending the second color in random mode too.
        if (mode == 0x10 || mode == 0x01)
            putColor(packet, 12, s.color2);
    }
    return {packet};
}

QByteArray lightingOff() {
    return report({0x51, 0x2c, 0x00, 0x00, 0xff, 0x04, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00});
}

QList<QByteArray> directFrame(const QList<QPair<int, QColor>> &leds) {
    QList<QByteArray> packets;
    for (int i = 0; i < leds.size(); i += DirectLedsPerReport) {
        const int count = std::min<int>(DirectLedsPerReport, leds.size() - i);
        QByteArray prefix;
        prefix.append(char(0xc0)).append(char(0x81)).append(char(std::min<int>(0xff, leds.size() - i))).append(char(0x00));
        for (int j = i; j < i + count; ++j) {
            const QColor c = leds[j].second.toRgb();
            prefix.append(char(leds[j].first & 0xff)).append(char(c.red())).append(char(c.green())).append(char(c.blue()));
        }
        packets.append(report(prefix));
    }
    return packets;
}

QByteArray directStart() {
    return report({0x51, 0x2c, 0x02, 0x00, 0x1f, 0x64, 0x00, 0xff, 0xff});
}

bool isDirect(const QByteArray &packet) {
    return packet.size() >= 2 && static_cast<unsigned char>(packet[0]) == 0xc0 && static_cast<unsigned char>(packet[1]) == 0x81;
}

QByteArray oledBrightness(int percent) { return report({0x68, 0x00, 0x00, 0x00, clampByte(percent, 0, 100)}); }
QByteArray oledPower(bool on) { return report({0x69, 0x00, 0x00, 0x00, on ? 1 : 0}); }
QByteArray oledPreset(int index) { return report({0x61, 0x00, 0x00, 0x00, clampByte(index, 0, 5)}); }

QList<QByteArray> oledUpload(const QList<QByteArray> &frames, const QList<int> &delays, int chunk) {
    QByteArray payload;
    const int count = frames.size();
    payload.append(static_cast<char>(count & 0xff));
    payload.append(static_cast<char>((count >> 8) & 0xff));
    for (int i = 0; i < count; ++i) {
        const int delay = qBound(0, i < delays.size() ? delays[i] : 100, 0xffff);
        payload.append(static_cast<char>(delay & 0xff));
        payload.append(static_cast<char>((delay >> 8) & 0xff));
    }
    for (const QByteArray &frame : frames)
        payload.append(frame.left(OledFrameBytes).leftJustified(OledFrameBytes, '\0', true));
    if (payload.size() % chunk)
        payload.append(QByteArray(chunk - payload.size() % chunk, '\0'));

    // The packet count and indexes are 16-bit little endian; Armoury's captures
    // only reached 222 packets, so the high byte is zero there.
    const int packets = payload.size() / chunk;
    QList<QByteArray> out;
    out.append(report({0x61, 0x01, 0x00, 0x00, packets & 0xff, (packets >> 8) & 0xff}));
    for (int i = 0; i < packets; ++i) {
        const int index = packets - 1 - i;
        QByteArray packet = report({0x61, 0x02, index & 0xff, (index >> 8) & 0xff});
        packet.replace(4, chunk, payload.mid(i * chunk, chunk));
        out.append(packet);
    }
    out.append(report({0x61, 0x03}));
    return out;
}

QByteArray remapKey(int key, int code, int param) {
    return report({0x51, 0x20, key, 0x00, code, param});
}

QByteArray knobFunction(int function, bool enabled) {
    return report({0x71, 0x00, 0x00, 0x00, function, enabled ? 1 : 0});
}

QByteArray knobCustom(int slot, int code, int param) {
    return report({0x71, 0x01, 0x00, 0x00, KnobCustom, slot, 0x00, code, param});
}

QByteArray sleepTimeout(int index) {
    return report({0x51, 0x38, 0x00, 0x00, index == SleepNever ? SleepNever : clampByte(index, 0, 4)});
}

QByteArray batteryAlert(int percent) { return report({0x51, 0x3a, 0x00, 0x00, clampByte(percent, 0, 100)}); }
QByteArray powerSaving(int mode) { return report({0x51, 0x39, 0x00, 0x00, clampByte(mode, 0, 2)}); }
QByteArray debounce(int milliseconds) { return report({0x6c, 0x00, 0x00, 0x00, clampByte(milliseconds, 15, 50)}); }

QByteArray clockTime(const QDateTime &when) {
    const QDate d = when.date();
    const QTime t = when.time();
    return report({0x63, 0x00, 0x00, 0x00, 0x01, d.year() & 0xff, (d.year() >> 8) & 0xff,
                   d.month(), d.day(), t.hour(), t.minute()});
}

QByteArray musicSpectrumStart(int style) {
    // Type 01 (spectrum only) carries no picture: 0 packets, 208x64.
    return report({0x67, 0x00, 0x00, 0x00, 0x01, clampByte(style, 1, 3), 0x00, 0x00, 0xd0, 0x00, 0x40, 0x00});
}

QByteArray musicBands(const QByteArray &bands) {
    return report(QByteArray::fromHex("67020000") + bands.left(ReportSize - 4));
}

QByteArray switchProfile(int profile) {
    return report({0x51, 0x00, 0x00, 0x00, clampByte(profile, 1, DefaultProfile)});
}

QString versionFromReply(const QByteArray &reply, bool receiver) {
    const int at = receiver ? 11 : 4;
    if (reply.size() < at + 3 || static_cast<unsigned char>(reply[0]) != 0x12 || reply[1] != 0x00)
        return {};
    // A sleeping keyboard behind the receiver leaves its fields zero.
    if (reply[at] == 0 && reply[at + 1] == 0 && reply[at + 2] == 0)
        return {};
    auto hex = [&](int i) { return QString::asprintf("%02X", static_cast<unsigned char>(reply[i])); };
    return QString::number(static_cast<unsigned char>(reply[at + 2])) + "." + hex(at + 1) + "." + hex(at);
}

int batteryFromReply(const QByteArray &reply) {
    if (reply.size() < 6 || static_cast<unsigned char>(reply[0]) != 0x12 || reply[1] != 0x01)
        return -1;
    return static_cast<unsigned char>(reply[5]);
}

bool isRejected(const QByteArray &reply) {
    return reply.size() >= 2 && static_cast<unsigned char>(reply[0]) == 0xff
        && static_cast<unsigned char>(reply[1]) == 0xaa;
}

} // namespace Protocol
