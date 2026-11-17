#pragma once

#include <QByteArray>
#include <QObject>
#include <QSettings>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <vector>

// Once a program has talked to the Azoth, the keyboard stops doing three of
// its knob functions itself (lighting brightness, OLED brightness, lighting
// effect) and only says the knob was turned, on its third interface, as
// Armoury Crate expects. This reads those reports.
namespace Knob {

struct Event {
    enum Kind { Nothing, Turned, FunctionChanged, Media };
    Kind kind = Nothing;
    int value = 0; // Turned: 1 up, -1 down. FunctionChanged: a Protocol::KnobFunction.
};

// One report from that interface. The click and letting go are Nothing; Media
// is a media key going down, which volume and media send with every turn.
Event parse(const QByteArray &report);

// The Azoth interfaces that carry the reports: vendor page 0xFFC0. Roots are
// injectable for tests.
QStringList inputNodes(const QString &sysRoot = "/sys", const QString &devRoot = "/dev");

} // namespace Knob

// Follows the knob and says when one of the functions left to the host turns.
class KnobListener : public QObject {
    Q_OBJECT

public:
    // The keyboard can't be asked which function is on, so the last one it
    // said is kept in the settings.
    explicit KnobListener(QSettings *settings, QObject *parent = nullptr);
    ~KnobListener() override;

    // Opens the keyboard's interface, again whenever it comes back.
    void start();
    // One report, and when it came in milliseconds; start() feeds what it reads.
    void feed(const QByteArray &report, qint64 nowMs);

signals:
    // function is one the keyboard leaves to the host; step is 1 up or -1 down.
    void turned(int function, int step);

private:
    void open();
    void close();
    void read(int fd);
    void setFunction(int function);

    QSettings *m_settings;
    int m_function = -1; // unknown until the keyboard says
    qint64 m_mediaAt = -1;
    struct Input {
        int fd;
        std::unique_ptr<QSocketNotifier> notifier;
    };
    std::vector<Input> m_inputs;
    QTimer m_retry; // the keyboard may come and go
};
