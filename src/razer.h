#pragma once

#include <QByteArray>
#include <QColor>
#include <QString>

#include "device.h"

// Razer mouse lighting, as OpenRazer's driver drives it (razercommon.c,
// razerchromacommon.c): 90-byte feature reports on the mouse's first
// interface. The DeathAdder V2 (1532:0084) uses the extended matrix commands
// with transaction id 0x3F, for its logo and scroll wheel.
namespace Razer {

constexpr int ReportSize = 90;
constexpr int TransactionId = 0x3f;

enum Led { ScrollWheel = 0x01, Logo = 0x04 };
enum Effect { Off, Static, Breathing, Spectrum, EffectCount };

struct Zone {
    int effect = Static;
    QColor color1 = QColor(0, 255, 0);
    QColor color2 = QColor(0, 0, 255);
    bool twoColors = false; // breathing between two colors instead of one
    int brightness = 100;   // 0-100
};

// Stored as "effect|#rrggbb|#rrggbb|two|brightness"; an unreadable entry gives ok = false.
QString encodeZone(const Zone &zone);
Zone decodeZone(const QString &text, bool *ok = nullptr);

// A report with its CRC (the XOR of bytes 2 to 87, stored at byte 88).
QByteArray report(int commandClass, int commandId, const QByteArray &arguments, int transaction = TransactionId);
// The effect report for one LED; stored on the mouse (VARSTORE).
QByteArray effect(int led, const Zone &zone);
QByteArray brightness(int led, int percent);
// Status 0x02 is done and 0x01 busy (still taken); anything else failed.
bool accepted(const QByteArray &request, const QByteArray &reply);

// How reports reach the mouse; a hidraw node in the app, a fake in tests.
class FeaturePort {
public:
    virtual ~FeaturePort() = default;
    virtual bool set(const QByteArray &report) = 0;
    virtual QByteArray get() = 0;
};

// Feature reports through /dev/hidrawN (HIDIOCSFEATURE / HIDIOCGFEATURE).
class HidrawFeaturePort : public FeaturePort {
public:
    explicit HidrawFeaturePort(int fd) : m_fd(fd) {}
    ~HidrawFeaturePort() override;
    static HidrawFeaturePort *open(const QString &path);
    bool set(const QByteArray &report) override;
    QByteArray get() override;

private:
    int m_fd;
};

class Mouse {
public:
    explicit Mouse(FeaturePort *port) : m_port(port) {}
    // Sets the zone's effect, then its brightness.
    bool apply(int led, const Zone &zone);

private:
    bool send(const QByteArray &request);
    FeaturePort *m_port;
};

// The DeathAdder V2's first interface (input0), which takes the reports.
DeviceNode find(const QString &sysRoot = "/sys/class/hidraw", const QString &devRoot = "/dev");

} // namespace Razer
