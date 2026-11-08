#include "oledimage.h"

#include <QCoreApplication>
#include <QImageReader>
#include <QPainter>

#include "protocol.h"

namespace OledImage {

QByteArray encodeFrame(const QImage &source) {
    const int width = Protocol::OledWidth, height = Protocol::OledHeight;
    QImage canvas(width, height, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    if (!source.isNull()) {
        const QImage scaled = source.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&canvas);
        painter.drawImage((width - scaled.width()) / 2, (height - scaled.height()) / 2, scaled);
    }

    // A 4x4 Bayer matrix spreads the rounding error, so 16 levels of gray
    // still show smooth gradients on the panel.
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    QByteArray frame(Protocol::OledFrameBytes, '\0');
    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(canvas.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const int gray = qGray(line[x]); // 0-255
            const int scaled = gray * 15 + (bayer[y % 4][x % 4] * 255) / 16 - 120;
            const int level = qBound(0, (scaled + 127) / 255, 15);
            char &byte = frame[y * Protocol::OledBytesPerRow + x / 2];
            byte = static_cast<char>(x % 2 == 0 ? (byte & 0x0f) | (level << 4) : (byte & 0xf0) | level);
        }
    }
    return frame;
}

QImage decodeFrame(const QByteArray &frame) {
    QImage image(Protocol::OledWidth, Protocol::OledHeight, QImage::Format_Grayscale8);
    for (int y = 0; y < Protocol::OledHeight; ++y) {
        uchar *line = image.scanLine(y);
        for (int x = 0; x < Protocol::OledWidth; ++x) {
            const int index = y * Protocol::OledBytesPerRow + x / 2;
            const int byte = index < frame.size() ? static_cast<unsigned char>(frame[index]) : 0;
            line[x] = static_cast<uchar>((x % 2 == 0 ? byte >> 4 : byte & 0x0f) * 17);
        }
    }
    return image;
}

Animation load(const QString &path) {
    Animation animation;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead()) {
        animation.error = QCoreApplication::translate("OledImage", "Can't read %1: %2").arg(path, reader.errorString());
        return animation;
    }
    while (animation.frames.size() < MaxFrames) {
        const QImage image = reader.read();
        if (image.isNull())
            break;
        animation.frames.append(encodeFrame(image));
        // GIFs without a delay play at Armoury's still-image pace.
        const int delay = reader.nextImageDelay();
        animation.delays.append(reader.supportsAnimation() ? (delay > 0 ? delay : 100) : 1000);
        if (!reader.supportsAnimation())
            break;
    }
    if (animation.frames.isEmpty())
        animation.error = QCoreApplication::translate("OledImage", "Can't read %1: %2").arg(path, reader.errorString());
    return animation;
}

} // namespace OledImage
