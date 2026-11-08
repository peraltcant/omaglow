#pragma once

#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include "design.h"

// Gathers what the live layers show, inside `omaglow lights`: the default
// output's audio through parec (as the music display does) and Hyprland's
// workspaces through its IPC socket. Each source runs only while a shown
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

    // Workspaces with windows from `j/workspaces` (special ones, with
    // negative ids, left out), and the id from `j/activeworkspace`, 0 if unreadable.
    static QSet<int> busyWorkspaces(const QByteArray &json);
    static int activeWorkspace(const QByteArray &json);
    // Hyprland's request socket: this session's, or the newest one running.
    static QString hyprlandSocket();

private:
    void pollWorkspaces();
    QByteArray askHyprland(const QByteArray &request) const;

    int m_needs = 0;
    QProcess m_recorder;
    QByteArray m_pending;
    QList<float> m_samples;
    QTimer m_workspaceTimer;
    QSet<int> m_busy;
    int m_active = 0;
};
