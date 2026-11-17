#pragma once

#include <QObject>
#include <QSettings>
#include <QTimer>
#include <functional>
#include <memory>

#include "backend.h"
#include "knob.h"

// Does the knob functions the keyboard leaves to the host while the app is
// closed; an open app listens itself and shows the change. Runs headless as
// `omaglow knob`, from omaglow-knob.service.
class KnobService : public QObject {
    Q_OBJECT

public:
    using BackendMaker = std::function<Backend *()>;
    using AppOpen = std::function<bool()>;

    KnobService(QSettings *settings, BackendMaker maker, AppOpen appOpen, QObject *parent = nullptr);

    void start();
    // One notch, as KnobListener reports it.
    void turned(int function, int step);
    // Sends what is pending and lets the backend go; runs a few seconds after
    // the last notch, so the next one reads the settings the app may have changed.
    void rest();

private:
    KnobListener m_listener;
    BackendMaker m_maker;
    AppOpen m_appOpen;
    std::unique_ptr<Backend> m_backend;
    QTimer m_idle;
};
