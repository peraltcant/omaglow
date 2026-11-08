#pragma once

#include <QByteArray>
#include <QString>

// A channel to the keyboard: write one 64-byte report and return the
// device's matching reply, or an empty array when none arrives in time.
class Link {
public:
    virtual ~Link() = default;
    virtual QByteArray transact(const QByteArray &packet) = 0;
    // Writes a report the keyboard doesn't answer, like all but one of a
    // direct mode frame's; false when the channel is gone.
    virtual bool post(const QByteArray &packet) { return !transact(packet).isEmpty(); }
    // How long transact() waits for an answer.
    virtual void setTimeoutMs(int) {}
};

// The vendor channel through a Linux hidraw node.
class HidrawLink : public Link {
public:
    explicit HidrawLink(int fd, int reportId = 0, int timeoutMs = 400)
        : m_fd(fd), m_reportId(reportId), m_timeoutMs(timeoutMs) {}
    ~HidrawLink() override;

    // Opens the node read-write; returns nullptr and sets errorNumber on failure.
    // The report ID is 0 on the cable and 2 through the ROG Omni receiver.
    static HidrawLink *open(const QString &path, int reportId = 0, int *errorNumber = nullptr);
    // Opens a /dev/hidrawN, reading its report ID from sysfs.
    static HidrawLink *openNode(const QString &path);

    // Through the receiver every report starts with its ID and carries 63
    // bytes of the keyboard's 64-byte command.
    static QByteArray frame(const QByteArray &packet, int reportId);
    static QByteArray unframe(const QByteArray &report, int reportId);
    // Whether a report answers the command: an echo of its first two bytes,
    // c0 82 for direct mode's c0 81, or ff aa for a command it doesn't know.
    static bool isReplyTo(const QByteArray &packet, const QByteArray &reply);

    QByteArray transact(const QByteArray &packet) override;
    bool post(const QByteArray &packet) override;
    bool failed() const { return m_failed; }
    void setTimeoutMs(int ms) override { m_timeoutMs = ms; }

private:
    void drain();
    int m_fd;
    int m_reportId;
    int m_timeoutMs;
    bool m_failed = false;
};

// Turns at the keyboard for every Omaglow process (the app and the light,
// clock and music services). Talking to it from two at once, say a battery
// check in the middle of a frame's reports, left it stuck until replugged.
// Held for a whole frame or a command and its reply; nests within a process.
class DeviceLock {
public:
    DeviceLock();
    ~DeviceLock();
    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;
    // In XDG_RUNTIME_DIR, so it belongs to the user and goes away at logout.
    static QString path();
};

struct DeviceNode {
    enum Status { NotFound, NoPermission, Found };
    Status status = NotFound;
    QString path;       // /dev/hidrawN
    QString productId;  // "1A83" wired, "1A85" 2.4 GHz dongle, "1ACE" ROG Omni receiver
    int reportId = 0;   // 0 on the cable, 2 through the ROG Omni receiver
};

namespace Discovery {

// Finds the Azoth's vendor interface among the hidraw nodes. The node number
// changes on every reconnect, so match by USB ID and by the report
// descriptor's usage page (0xFF00) instead. Roots are injectable for tests.
DeviceNode find(const QString &sysRoot = "/sys/class/hidraw", const QString &devRoot = "/dev");

bool isAzoth(const QString &uevent, QString *productId = nullptr);
bool isVendorDescriptor(const QByteArray &descriptor);

// The report ID of the keyboard's vendor collection (usage page 0xFF00):
// 0 when the interface has no report IDs, -1 when there is no such collection.
int vendorReportId(const QByteArray &descriptor);

} // namespace Discovery
