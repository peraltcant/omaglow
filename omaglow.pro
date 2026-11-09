QT += core gui qml quick quickcontrols2 dbus concurrent network

CONFIG += c++17 release
TARGET = omaglow
TEMPLATE = app

# The one place the version is written; the PKGBUILD's pkgver must match it
# (bin/check-version, run in CI).
OMAGLOW_VERSION = $$cat($$PWD/VERSION)
DEFINES += OMAGLOW_VERSION=\\\"$$OMAGLOW_VERSION\\\"

HEADERS += \
    src/live.h \
    src/backend.h \
    src/device.h \
    src/clockservice.h \
    src/design.h \
    src/lightservice.h \
    src/musicservice.h \
    src/spectrum.h \
    src/keymap.h \
    src/filepicker.h \
    src/oledimage.h \
    src/portalfilepicker.h \
    src/protocol.h \
    src/theme.h \
    src/translator.h \
    src/razer.h \
    src/laptop.h \
    src/appinfo.h

SOURCES += \
    src/live.cpp \
    src/main.cpp \
    src/backend.cpp \
    src/device.cpp \
    src/clockservice.cpp \
    src/design.cpp \
    src/lightservice.cpp \
    src/musicservice.cpp \
    src/spectrum.cpp \
    src/keymap.cpp \
    src/oledimage.cpp \
    src/portalfilepicker.cpp \
    src/protocol.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/razer.cpp \
    src/laptop.cpp \
    src/appinfo.cpp

RESOURCES += src/resources.qrc
