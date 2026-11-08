#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>

// Turns pictures into the OLED's format: 208x64 pixels, 16 gray levels,
// two pixels per byte with the left one in the high nibble.
namespace OledImage {

constexpr int MaxFrames = 196; // Armoury's limit for animations.

struct Animation {
    QList<QByteArray> frames;
    QList<int> delays; // milliseconds per frame
    QString error;
};

// Fits the picture inside 208x64 keeping its proportions, centered on black,
// and maps luminance to 16 levels with ordered dithering so gradients survive.
QByteArray encodeFrame(const QImage &image);

// The frame as a picture again, scaled for display in the window.
QImage decodeFrame(const QByteArray &frame);

// Reads a still image or every frame of an animated GIF (up to MaxFrames).
Animation load(const QString &path);

} // namespace OledImage
