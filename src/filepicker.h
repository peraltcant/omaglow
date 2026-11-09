#pragma once

#include <QObject>
#include <QUrl>

// Opens files through an injectable picker, so tests can fake the portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openImage() = 0;
    // A look file (.json) to import, and where to save one, offering name.
    virtual void openLook() = 0;
    virtual void saveLook(const QString &name) = 0;

signals:
    void openSelected(const QUrl &url);
    void lookOpened(const QUrl &url);
    void lookSaveChosen(const QUrl &url);
    void failed(const QString &message);
};
