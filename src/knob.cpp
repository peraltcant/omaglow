#include "knob.h"

#include "device.h"
#include "protocol.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {

const char functionKey[] = "knob/function";
// A media key this close before a turn came from the same notch.
constexpr int mediaWindowMs = 50;
constexpr int retryMs = 3000;

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool hostFunction(int function) {
    return function == Protocol::KnobKeyboardBrightness || function == Protocol::KnobOledBrightness ||
           function == Protocol::KnobEffect;
}

} // namespace

Knob::Event Knob::parse(const QByteArray &report) {
    const auto byte = [&](int i) { return i < report.size() ? quint8(report.at(i)) : 0; };
    if (byte(0) == 0x01) {
        for (int i = 1; i < report.size(); ++i)
            if (report.at(i))
                return {Event::Media, 0};
        return {};
    }
    if (byte(0) != 0x03)
        return {};
    if (byte(1) == 0x91)
        return {Event::FunctionChanged, byte(4)};
    if (byte(1) == 0x72) {
        if (byte(2) == 0x01)
            return {Event::Turned, 1};
        if (byte(2) == 0x04)
            return {Event::Turned, -1};
    }
    return {};
}

QStringList Knob::inputNodes(const QString &sysRoot, const QString &devRoot) {
    QStringList nodes;
    const QDir hidraw(sysRoot + "/class/hidraw");
    for (const QString &name : hidraw.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!Discovery::isAzoth(QString::fromLatin1(readFile(hidraw.filePath(name + "/device/uevent")))))
            continue;
        // Usage page 0xFFC0, as the descriptor writes it.
        if (readFile(hidraw.filePath(name + "/device/report_descriptor")).contains(QByteArray::fromHex("06c0ff")))
            nodes.append(devRoot + "/" + name);
    }
    return nodes;
}

KnobListener::KnobListener(QSettings *settings, QObject *parent) : QObject(parent), m_settings(settings) {
    m_function = m_settings->value(functionKey, -1).toInt();
    m_retry.setInterval(retryMs);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_inputs.empty())
            open();
    });
}

KnobListener::~KnobListener() {
    close();
}

void KnobListener::start() {
    open();
    m_retry.start();
}

void KnobListener::open() {
    close();
    for (const QString &path : Knob::inputNodes()) {
        // Read only: each open file gets its own copy of every report.
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        Input input{fd, std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read)};
        connect(input.notifier.get(), &QSocketNotifier::activated, this, [this, fd] { read(fd); });
        m_inputs.push_back(std::move(input));
    }
}

void KnobListener::close() {
    for (Input &input : m_inputs) {
        input.notifier.reset();
        ::close(input.fd);
    }
    m_inputs.clear();
}

void KnobListener::read(int fd) {
    char buffer[64];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) {
            // Gone (unplugged): the retry opens it again.
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                QMetaObject::invokeMethod(this, [this] { close(); }, Qt::QueuedConnection);
            return;
        }
        feed(QByteArray(buffer, int(n)), QDateTime::currentMSecsSinceEpoch());
    }
}

void KnobListener::setFunction(int function) {
    if (function == m_function)
        return;
    m_function = function;
    if (function < 0)
        m_settings->remove(functionKey);
    else
        m_settings->setValue(functionKey, function);
}

void KnobListener::feed(const QByteArray &report, qint64 nowMs) {
    const Knob::Event event = Knob::parse(report);
    switch (event.kind) {
    case Knob::Event::Media:
        m_mediaAt = nowMs;
        break;
    case Knob::Event::FunctionChanged:
        setFunction(event.value);
        break;
    case Knob::Event::Turned:
        if (m_mediaAt >= 0 && nowMs - m_mediaAt <= mediaWindowMs) {
            // The keyboard did this turn itself (volume or media), so the
            // function kept from before is wrong.
            if (hostFunction(m_function))
                setFunction(-1);
        } else if (hostFunction(m_function)) {
            emit turned(m_function, event.value);
        }
        break;
    case Knob::Event::Nothing:
        break;
    }
}
