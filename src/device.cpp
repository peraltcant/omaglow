#include "device.h"

#include "appinfo.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRecursiveMutex>
#include <QRegularExpression>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>

HidrawLink::~HidrawLink() {
    if (m_fd >= 0)
        ::close(m_fd);
}

HidrawLink *HidrawLink::open(const QString &path, int reportId, int *errorNumber) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errorNumber)
            *errorNumber = errno;
        return nullptr;
    }
    return new HidrawLink(fd, reportId);
}

HidrawLink *HidrawLink::openNode(const QString &path) {
    // Without the descriptor the framing is unknown, and guessing the cable's
    // would send unframed reports to the receiver; refuse instead.
    QFile descriptor("/sys/class/hidraw/" + QFileInfo(path).fileName() + "/device/report_descriptor");
    if (!descriptor.open(QIODevice::ReadOnly))
        return nullptr;
    const int id = Discovery::vendorReportId(descriptor.readAll());
    return id < 0 ? nullptr : open(path, id);
}

QByteArray HidrawLink::frame(const QByteArray &packet, int reportId) {
    if (reportId <= 0)
        return packet;
    return QByteArray(1, char(reportId)) + packet.left(63).leftJustified(63, '\0');
}

QByteArray HidrawLink::unframe(const QByteArray &report, int reportId) {
    if (reportId <= 0 || report.isEmpty() || static_cast<unsigned char>(report[0]) != reportId)
        return report;
    return report.mid(1);
}

// The keyboard also sends unsolicited reports (status echoes, clock acks), so
// old input is thrown away before each command.
void HidrawLink::drain() {
    char buffer[64];
    while (::read(m_fd, buffer, sizeof buffer) > 0) {
    }
}

bool HidrawLink::isReplyTo(const QByteArray &packet, const QByteArray &reply) {
    if (packet.size() < 2 || reply.size() < 2)
        return false;
    const auto byte = [](const QByteArray &b, int i) { return static_cast<unsigned char>(b[i]); };
    if (byte(reply, 0) == 0xff && byte(reply, 1) == 0xaa)
        return true;
    if (byte(packet, 0) == 0xc0 && byte(packet, 1) == 0x81)
        return byte(reply, 0) == 0xc0 && byte(reply, 1) == 0x82;
    return reply[0] == packet[0] && reply[1] == packet[1];
}

bool HidrawLink::post(const QByteArray &packet) {
    if (m_failed)
        return false;
    drain();
    const QByteArray out = frame(packet, m_reportId);
    if (::write(m_fd, out.constData(), out.size()) != out.size())
        m_failed = true;
    return !m_failed;
}

QByteArray HidrawLink::transact(const QByteArray &packet) {
    if (m_failed)
        return {};
    drain();
    const QByteArray out = frame(packet, m_reportId);
    if (::write(m_fd, out.constData(), out.size()) != out.size()) {
        m_failed = true;
        return {};
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < m_timeoutMs) {
        pollfd pfd{m_fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, int(m_timeoutMs - timer.elapsed()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            m_failed = true;
            return {};
        }
        char buffer[64];
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n < 2)
            continue;
        if (m_reportId > 0 && static_cast<unsigned char>(buffer[0]) != m_reportId)
            continue; // another collection on the receiver (its own or a mouse's)
        const QByteArray reply = unframe(QByteArray(buffer, int(n)), m_reportId);
        if (reply.size() < 2)
            continue;
        if (isReplyTo(packet, reply))
            return reply;
    }
    return {};
}

namespace Discovery {

bool isAzoth(const QString &uevent, QString *productId) {
    static const QRegularExpression id(R"(HID_ID=0003:00000B05:0000(1A83|1A85|1ACE))",
                                       QRegularExpression::CaseInsensitiveOption);
    const auto match = id.match(uevent);
    if (match.hasMatch() && productId)
        *productId = match.captured(1).toUpper();
    return match.hasMatch();
}

bool isVendorDescriptor(const QByteArray &descriptor) {
    return vendorReportId(descriptor) >= 0;
}

int vendorReportId(const QByteArray &descriptor) {
    // Usage Page (Vendor Defined 0xFF00), Usage 1, Collection, then either a
    // Report ID item (85 id, as on the Omni receiver) or none (the cable).
    static const QByteArray head = QByteArray::fromHex("0600ff0901a101");
    int at = descriptor.indexOf(head);
    // A collection starts the descriptor or follows the end of another one
    // (c0); the same bytes elsewhere are data of some other item.
    while (at > 0 && static_cast<unsigned char>(descriptor[at - 1]) != 0xc0)
        at = descriptor.indexOf(head, at + 1);
    if (at < 0)
        return -1;
    const int next = at + head.size();
    if (next + 1 < descriptor.size() && static_cast<unsigned char>(descriptor[next]) == 0x85)
        return static_cast<unsigned char>(descriptor[next + 1]);
    return 0;
}

DeviceNode find(const QString &sysRoot, const QString &devRoot) {
    DeviceNode result;
    const auto entries = QDir(sysRoot).entryList({"hidraw*"}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : entries) {
        QFile uevent(sysRoot + "/" + name + "/device/uevent");
        QFile descriptor(sysRoot + "/" + name + "/device/report_descriptor");
        if (!uevent.open(QIODevice::ReadOnly) || !descriptor.open(QIODevice::ReadOnly))
            continue;
        QString productId;
        if (!isAzoth(QString::fromUtf8(uevent.readAll()), &productId) || !isVendorDescriptor(descriptor.readAll()))
            continue;

        result.path = devRoot + "/" + name;
        result.productId = productId;
        descriptor.seek(0);
        result.reportId = qMax(0, vendorReportId(descriptor.readAll()));
        const QFileInfo node(result.path);
        result.status = node.exists() && node.isReadable() && node.isWritable() ? DeviceNode::Found
                                                                                 : DeviceNode::NoPermission;
        // A wired connection wins over the dongle if both are present.
        if (result.status == DeviceNode::Found && productId == "1A83")
            return result;
    }
    return result;
}

} // namespace Discovery

namespace {
QRecursiveMutex lockMutex; // threads of one process take turns here
int lockFd = -1;
int lockDepth = 0;
QString lockPath;
}

QString DeviceLock::path() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return (runtime.isEmpty() ? QDir::tempPath() : runtime) + "/" + QString(AppInfo::Name) + "-keyboard.lock";
}

DeviceLock::DeviceLock() {
    lockMutex.lock();
    if (lockDepth++ > 0)
        return;
    const QString path = DeviceLock::path();
    if (lockFd >= 0 && path != lockPath) {
        ::close(lockFd);
        lockFd = -1;
    }
    if (lockFd < 0) {
        lockFd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        lockPath = path;
    }
    // Without the file the keyboard is still usable, only without turns.
    if (lockFd >= 0)
        while (::flock(lockFd, LOCK_EX) < 0 && errno == EINTR) {
        }
}

DeviceLock::~DeviceLock() {
    if (--lockDepth == 0 && lockFd >= 0)
        ::flock(lockFd, LOCK_UN);
    lockMutex.unlock();
}
