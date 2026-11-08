#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

#include "device.h"

// Keeps the OLED clock right. The keyboard has no clock of its own: it shows
// the last time it was sent and stays frozen there, so this pushes the time
// as each minute starts, finding the keyboard again when it comes back.
// Runs headless as `omaglow clock`, started by omaglow-clock.service.
class ClockService : public QObject {
    Q_OBJECT

public:
    using Finder = std::function<DeviceNode()>;
    using Opener = std::function<Link *(const QString &path)>;
    using Now = std::function<QDateTime()>;

    explicit ClockService(QObject *parent = nullptr);
    ClockService(Finder finder, Opener opener, Now now, QObject *parent = nullptr);

    void start();
    void tick();
    void dropLink();
    int millisecondsToNextMinute() const;

    // Sees every packet sent; for tests.
    void setObserver(std::function<void(const QByteArray &)> observer) { m_observer = std::move(observer); }

private:
    Finder m_finder;
    Opener m_opener;
    Now m_now;
    std::unique_ptr<Link> m_link;
    QTimer m_timer;
    std::function<void(const QByteArray &)> m_observer;
    QDateTime m_lastSent; // minute shown on the display, invalid when unknown
};
