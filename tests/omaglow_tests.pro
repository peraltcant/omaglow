QT += core gui quick quickcontrols2 testlib dbus concurrent network
CONFIG += c++17 testcase
TARGET = omaglow_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/device.h ../src/spectrum.h ../src/musicservice.h ../src/clockservice.h ../src/design.h ../src/lightservice.h ../src/keymap.h ../src/filepicker.h ../src/oledimage.h \
           ../src/portalfilepicker.h ../src/protocol.h ../src/theme.h ../src/translator.h ../src/razer.h ../src/laptop.h ../src/appinfo.h
SOURCES += omaglow_tests.cpp ../src/backend.cpp ../src/device.cpp ../src/spectrum.cpp ../src/musicservice.cpp ../src/clockservice.cpp ../src/design.cpp ../src/lightservice.cpp ../src/keymap.cpp ../src/oledimage.cpp \
           ../src/portalfilepicker.cpp ../src/protocol.cpp ../src/theme.cpp ../src/translator.cpp ../src/razer.cpp ../src/laptop.cpp ../src/appinfo.cpp
