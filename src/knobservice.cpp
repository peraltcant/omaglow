#include "knobservice.h"

namespace {
constexpr int idleMs = 5000;
}

KnobService::KnobService(QSettings *settings, BackendMaker maker, AppOpen appOpen, QObject *parent)
    : QObject(parent), m_listener(settings), m_maker(std::move(maker)), m_appOpen(std::move(appOpen)) {
    m_idle.setSingleShot(true);
    m_idle.setInterval(idleMs);
    connect(&m_idle, &QTimer::timeout, this, &KnobService::rest);
    connect(&m_listener, &KnobListener::turned, this, &KnobService::turned);
}

void KnobService::start() {
    m_listener.start();
}

void KnobService::turned(int function, int step) {
    if (m_appOpen()) {
        rest();
        return;
    }
    if (!m_backend)
        m_backend.reset(m_maker());
    m_backend->knobTurned(function, step);
    m_idle.start();
}

void KnobService::rest() {
    m_idle.stop();
    if (!m_backend)
        return;
    m_backend->flush();
    m_backend.reset();
}
