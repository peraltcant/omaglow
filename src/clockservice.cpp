#include "clockservice.h"

#include "protocol.h"

namespace {
// Lands just after the minute changes, so the display never lags a minute.
constexpr int afterMinuteMs = 200;
// Checked this often too: Qt timers stop during suspend and don't follow
// clock changes, so a missed minute or a jump is caught within seconds.
// The keyboard is only written to when the minute differs.
constexpr int checkMs = 5000;
}

ClockService::ClockService(QObject *parent)
    : ClockService([] { return Discovery::find(); },
                   [](const QString &path) -> Link * { return HidrawLink::openNode(path); },
                   [] { return QDateTime::currentDateTime(); }, parent) {}

ClockService::ClockService(Finder finder, Opener opener, Now now, QObject *parent)
    : QObject(parent), m_finder(std::move(finder)), m_opener(std::move(opener)), m_now(std::move(now)) {
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        tick();
        m_timer.start(qMin(checkMs, millisecondsToNextMinute()));
    });
}

void ClockService::start() {
    tick();
    m_timer.start(qMin(checkMs, millisecondsToNextMinute()));
}

void ClockService::dropLink() {
    m_link.reset();
    m_lastSent = {};
}

int ClockService::millisecondsToNextMinute() const {
    const QTime time = m_now().time();
    return (60 - time.second()) * 1000 - time.msec() + afterMinuteMs;
}

void ClockService::tick() {
    if (!m_link) {
        const DeviceNode node = m_finder();
        if (node.status != DeviceNode::Found)
            return;
        m_link.reset(m_opener(node.path));
        if (!m_link)
            return;
        m_lastSent = {};
    }
    QDateTime minute = m_now();
    minute.setTime(QTime(minute.time().hour(), minute.time().minute()));
    if (minute == m_lastSent)
        return;
    const QByteArray packet = Protocol::clockTime(minute);
    if (m_observer)
        m_observer(packet);
    QByteArray reply;
    {
        const DeviceLock lock;
        reply = m_link->transact(packet);
    }
    if (!reply.isEmpty()) {
        m_lastSent = minute;
        return;
    }
    // A keyboard that stopped answering was unplugged or grabbed by a VM;
    // look for it again on the next tick. A plain timeout (a sleeping
    // wireless keyboard) keeps the link and tries again.
    if (auto *hid = dynamic_cast<HidrawLink *>(m_link.get()); hid && hid->failed())
        dropLink();
}
