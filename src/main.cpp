// Omaglow (formerly azoth): lighting, OLED and keys for the ROG Azoth, plus the
// Razer mouse and the ASUS laptop keyboard, on Omarchy.

#include <QDir>
#include <QGuiApplication>
#include <QLockFile>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QUrl>

#include "appinfo.h"
#include "backend.h"
#include "clockservice.h"
#include "lightservice.h"
#include "musicservice.h"
#include <QSettings>
#include "theme.h"
#include "translator.h"

// Serves "image://oled/<frame>" so QML can show what the OLED will display.
class OledPreviewProvider : public QQuickImageProvider {
public:
    explicit OledPreviewProvider(Backend *backend)
        : QQuickImageProvider(QQuickImageProvider::Image), m_backend(backend) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        const QImage image = m_backend->previewFrame(id.section('?', 0, 0).toInt());
        if (size)
            *size = image.size();
        return image;
    }

private:
    Backend *m_backend;
};

static QString appLockPath() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return (runtime.isEmpty() ? QDir::tempPath() : runtime) + "/" + QString(AppInfo::Name) + "-app.lock";
}

int main(int argc, char *argv[]) {
    // Settings saved under the old name come along on the first run of any mode.
    AppInfo::migrateLegacySettings();

    // `omaglow clock` keeps the OLED clock right with no window; the
    // omaglow-clock.service user unit runs it while the clock is chosen.
    if (argc > 1 && QByteArray(argv[1]) == "clock") {
        QCoreApplication app(argc, argv);
        app.setApplicationName(AppInfo::Name);
        app.setOrganizationName(AppInfo::Name);
        ClockService clock;
        clock.start();
        return app.exec();
    }

    // `omaglow music` streams spectrum bars of what is playing; the
    // omaglow-music.service user unit runs it while the music display is chosen.
    if (argc > 1 && QByteArray(argv[1]) == "music") {
        QCoreApplication app(argc, argv);
        app.setApplicationName(AppInfo::Name);
        app.setOrganizationName(AppInfo::Name);
        MusicService music(QSettings().value("oled/musicStyle", 1).toInt());
        music.start();
        return app.exec();
    }

    // `omaglow lights` streams moving key layers; the omaglow-lights.service
    // user unit runs it while the lighting moves.
    if (argc > 1 && QByteArray(argv[1]) == "lights") {
        QCoreApplication app(argc, argv);
        app.setApplicationName(AppInfo::Name);
        app.setOrganizationName(AppInfo::Name);
        LightService lights;
        lights.start();
        return app.exec();
    }

    // `omaglow theme-sync` runs from Omarchy's theme-set hook: with the app
    // closed it paints every device in the new theme's keyboard color. An
    // open app does that itself, so this leaves it alone.
    if (argc > 1 && QByteArray(argv[1]) == "theme-sync") {
        QCoreApplication app(argc, argv);
        app.setApplicationName(AppInfo::Name);
        app.setOrganizationName(AppInfo::Name);
        QLockFile open(appLockPath());
        if (!open.tryLock(0))
            return 0;
        Backend backend;
        if (backend.followTheme()) {
            backend.applyThemeColor(Theme::readKeyboardColor({}));
            backend.flush();
        }
        return 0;
    }

    QGuiApplication app(argc, argv);
    // Tells `omaglow theme-sync` the app is open; released when it quits.
    QLockFile appLock(appLockPath());
    appLock.tryLock(0);
    app.setApplicationName(AppInfo::Name);
    app.setOrganizationName(AppInfo::Name);
    app.setApplicationVersion("0.26.0");

    // Matches the window to omaglow.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName(AppInfo::Name);
    app.setWindowIcon(QIcon::fromTheme(AppInfo::Name));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    backend.syncClockService();
    backend.syncLightsService();
    // A theme picked while the app was closed and its hook missed shows now.
    const auto followTheme = [&] {
        if (backend.followTheme())
            backend.applyThemeColor(theme.keyboardColor());
    };
    // Rewrites the theme-set hook, replacing the one azoth installed.
    if (backend.followTheme())
        backend.setFollowTheme(true);
    QObject::connect(&theme, &Theme::changed, &backend, followTheme);
    QObject::connect(&backend, &Backend::followThemeChanged, &backend, followTheme);
    followTheme();

    QQmlApplicationEngine engine;
    engine.addImageProvider("oled", new OledPreviewProvider(&backend));
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
