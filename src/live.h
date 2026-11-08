#pragma once

#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QSocketNotifier>
#include <memory>
#include <vector>
#include <QTimer>
#include <functional>

#include "design.h"

// Gathers what the live layers show, inside `omaglow lights`: the default
// output's audio through parec (as the music display does), Hyprland's
// workspaces through its IPC socket, CPU load, temperature and battery from
// /proc and sysfs, notifications through dbus-monitor (only that one came;
// what it says is read past, never kept), and which keys are pressed from
// the keyboard's own key interfaces (kept a few seconds in memory to draw the
// ripples, never stored or logged). Each source runs only while a shown
// layer needs it.
class LiveFeed : public QObject {
    Q_OBJECT

public:
    explicit LiveFeed(QObject *parent = nullptr);
    ~LiveFeed() override;

    // LiveNeed flags; sources start and stop to match.
    void setNeeds(int needs);
    int needs() const { return m_needs; }
    Design::Live snapshot() const;
    // The clock notifications are timed on: the render clock.
    void setClock(std::function<qint64()> clock) { m_clockNow = std::move(clock); }

    // Workspaces with windows from `j/workspaces` (special ones, with
    // negative ids, left out), and the id from `j/activeworkspace`, 0 if unreadable.
    static QSet<int> busyWorkspaces(const QByteArray &json);
    static int activeWorkspace(const QByteArray &json);
    // Hyprland's request socket: this session's, or the newest one running.
    static QString hyprlandSocket();

    // Busy and total jiffies from /proc/stat's first line, and the busy
    // share between two readings.
    struct CpuTimes { qint64 busy = 0, total = 0; };
    static CpuTimes cpuTimes(const QByteArray &stat);
    static double cpuBusy(const CpuTimes &before, const CpuTimes &after);
    // The CPU package temperature in °C and the first battery's charge in %,
    // under a sysfs root; -1 when there is none.
    static double readTemperature(const QString &sysRoot = "/sys");
    static double readBattery(const QString &sysRoot = "/sys");

    // The usages held down in a key report: an 8-byte boot report
    // (modifiers, reserved, six keys) or a bitmap with one bit per usage.
    static QSet<int> pressedUsages(const QByteArray &report);
    // The Azoth's key interfaces (keyboard collections without report ids),
    // as /dev paths.
    static QStringList keyboardInputNodes(const QString &sysRoot = "/sys", const QString &devRoot = "/dev");

private:
    void pollWorkspaces();
    void pollSystem();
    void openKeyInputs();
    void closeKeyInputs();
    void readKeys(int fd);
    QByteArray askHyprland(const QByteArray &request) const;

    int m_needs = 0;
    QProcess m_recorder;
    QByteArray m_pending;
    QList<float> m_samples;
    QTimer m_workspaceTimer;
    QSet<int> m_busy;
    int m_active = 0;
    QTimer m_systemTimer;
    CpuTimes m_lastCpu;
    double m_cpu = -1, m_temperature = -1, m_battery = -1;
    QProcess m_monitor;
    QByteArray m_monitorLine;
    qint64 m_notifiedAt = -1;
    std::function<qint64()> m_clockNow;
    struct KeyInput {
        int fd;
        std::unique_ptr<QSocketNotifier> notifier;
        QSet<int> held;
    };
    std::vector<KeyInput> m_keyInputs;
    QTimer m_keyInputRetry; // the keyboard may come and go
    QHash<int, qint64> m_pressedAt;
};
