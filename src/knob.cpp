#include "knob.h"

#include "device.h"
#include "protocol.h"

#include <QDebug>
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
    m_clock.start();
    m_retry.setInterval(retryMs);
    // Every time, not only with nothing open: the keyboard can be on its
    // receiver and get its cable too.
    connect(&m_retry, &QTimer::timeout, this, &KnobListener::open);
}

KnobListener::~KnobListener() {
    while (!m_inputs.empty())
        close(m_inputs.back().fd);
}

void KnobListener::setRoots(const QString &sysRoot, const QString &devRoot, int retryMs) {
    m_sysRoot = sysRoot;
    m_devRoot = devRoot;
    m_retry.setInterval(retryMs);
}

void KnobListener::start() {
    open();
    m_retry.start();
}

void KnobListener::open() {
    for (const QString &path : Knob::inputNodes(m_sysRoot, m_devRoot)) {
        if (std::any_of(m_inputs.cbegin(), m_inputs.cend(), [&](const Input &input) { return input.path == path; }))
            continue;
        // Read only: each open file gets its own copy of every report.
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (!m_warned.contains(path))
                qWarning().noquote() << "can't read the knob from" << path + ":" << qt_error_string(errno);
            m_warned.insert(path);
            continue;
        }
        m_warned.remove(path);
        Input input{path, fd, std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read)};
        connect(input.notifier.get(), &QSocketNotifier::activated, this, [this, fd] { read(fd); });
        m_inputs.push_back(std::move(input));
    }
}

void KnobListener::close(int fd) {
    const auto input = std::find_if(m_inputs.begin(), m_inputs.end(), [fd](const Input &i) { return i.fd == fd; });
    if (input == m_inputs.end())
        return;
    input->notifier.reset();
    ::close(fd);
    m_inputs.erase(input);
}

void KnobListener::read(int fd) {
    char buffer[64];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) {
            // Gone (unplugged): the retry opens it again.
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                QMetaObject::invokeMethod(this, [this, fd] { close(fd); }, Qt::QueuedConnection);
            return;
        }
        feed(QByteArray(buffer, int(n)), m_clock.elapsed());
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
        m_said = true;
        setFunction(event.value);
        break;
    case Knob::Event::Turned:
        // A function only remembered from an earlier run may be wrong: a media
        // key sent with the turn means the keyboard did it itself (volume or
        // media). One the keyboard said in this run is trusted.
        if (!m_said && m_mediaAt >= 0 && nowMs - m_mediaAt <= mediaWindowMs) {
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
