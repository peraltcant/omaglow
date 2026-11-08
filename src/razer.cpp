#include "razer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Razer {

namespace {
constexpr int VarStore = 0x01;
QByteArray bytes(std::initializer_list<int> values) {
    QByteArray out;
    for (int v : values)
        out.append(char(v));
    return out;
}
// OpenRazer waits this long between sending a report and reading the answer.
constexpr int replyWaitUs = 600;
}

QString encodeZone(const Zone &z) {
    return QString("%1|%2|%3|%4|%5").arg(z.effect).arg(z.color1.name(), z.color2.name()).arg(z.twoColors ? 1 : 0).arg(z.brightness);
}

Zone decodeZone(const QString &text, bool *ok) {
    const QStringList f = text.split('|');
    Zone z;
    bool good = f.size() == 5;
    if (good) {
        bool e = false, b = false;
        z.effect = f[0].toInt(&e);
        z.color1 = QColor(f[1]);
        z.color2 = QColor(f[2]);
        z.twoColors = f[3] == "1";
        z.brightness = qBound(0, f[4].toInt(&b), 100);
        good = e && b && z.effect >= 0 && z.effect < EffectCount && z.color1.isValid() && z.color2.isValid();
    }
    if (ok)
        *ok = good;
    return good ? z : Zone{};
}

QByteArray report(int commandClass, int commandId, const QByteArray &arguments, int transaction) {
    QByteArray r(ReportSize, '\0');
    r[1] = char(transaction);
    r[5] = char(qMin<int>(arguments.size(), 80));
    r[6] = char(commandClass);
    r[7] = char(commandId);
    for (int i = 0; i < qMin<int>(arguments.size(), 80); ++i)
        r[8 + i] = arguments[i];
    char crc = 0;
    for (int i = 2; i < 88; ++i)
        crc ^= r[i];
    r[88] = crc;
    return r;
}

QByteArray effect(int led, const Zone &zone) {
    const auto rgb = [](const QColor &c) {
        const QColor x = c.toRgb();
        return bytes({x.red(), x.green(), x.blue()});
    };
    QByteArray args = bytes({VarStore, led});
    switch (zone.effect) {
    case Static:
        args += QByteArray("\x01\x00\x00\x01", 4) + rgb(zone.color1);
        break;
    case Breathing:
        if (zone.twoColors)
            args += QByteArray("\x02\x02\x00\x02", 4) + rgb(zone.color1) + rgb(zone.color2);
        else
            args += QByteArray("\x02\x01\x00\x01", 4) + rgb(zone.color1);
        break;
    case Spectrum:
        args += QByteArray("\x03\x00\x00\x00", 4);
        break;
    default: // off
        args += QByteArray(4, '\0');
        break;
    }
    return report(0x0f, 0x02, args);
}

QByteArray brightness(int led, int percent) {
    const int value = qRound(qBound(0, percent, 100) * 255 / 100.0);
    return report(0x0f, 0x04, bytes({VarStore, led, value}));
}

bool accepted(const QByteArray &request, const QByteArray &reply) {
    if (reply.size() < 8 || request.size() < 8)
        return false;
    const auto status = static_cast<unsigned char>(reply[0]);
    return (status == 0x01 || status == 0x02) && reply[6] == request[6] && reply[7] == request[7];
}

HidrawFeaturePort::~HidrawFeaturePort() {
    if (m_fd >= 0)
        ::close(m_fd);
}

HidrawFeaturePort *HidrawFeaturePort::open(const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC);
    return fd < 0 ? nullptr : new HidrawFeaturePort(fd);
}

// The report ID (0) goes first; the mouse's report has none of its own.
bool HidrawFeaturePort::set(const QByteArray &r) {
    QByteArray buffer = QByteArray(1, '\0') + r;
    return ::ioctl(m_fd, HIDIOCSFEATURE(buffer.size()), buffer.data()) >= 0;
}

QByteArray HidrawFeaturePort::get() {
    QByteArray buffer(ReportSize + 1, '\0');
    const int n = ::ioctl(m_fd, HIDIOCGFEATURE(buffer.size()), buffer.data());
    if (n < ReportSize + 1)
        return {};
    return buffer.mid(1, ReportSize);
}

bool Mouse::send(const QByteArray &request) {
    if (!m_port || !m_port->set(request))
        return false;
    QThread::usleep(replyWaitUs);
    return accepted(request, m_port->get());
}

bool Mouse::apply(int led, const Zone &zone) {
    return send(effect(led, zone)) && send(brightness(led, zone.brightness));
}

DeviceNode find(const QString &sysRoot, const QString &devRoot) {
    static const QRegularExpression id(R"(HID_ID=0003:00001532:0000(0084)\b)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression firstInterface(R"(HID_PHYS=\S*/input0\s)");
    DeviceNode result;
    const auto entries = QDir(sysRoot).entryList({"hidraw*"}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : entries) {
        QFile uevent(sysRoot + "/" + name + "/device/uevent");
        if (!uevent.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(uevent.readAll()) + "\n";
        const auto match = id.match(text);
        if (!match.hasMatch() || !firstInterface.match(text).hasMatch())
            continue;
        result.path = devRoot + "/" + name;
        result.productId = match.captured(1);
        const QFileInfo node(result.path);
        result.status = node.exists() && node.isReadable() && node.isWritable() ? DeviceNode::Found
                                                                                 : DeviceNode::NoPermission;
        return result;
    }
    return result;
}

} // namespace Razer
