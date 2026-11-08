#pragma once

#include <QByteArray>
#include <QList>

// Turns audio into the OLED's spectrum bars: 26 bands (208 pixels, 8 each)
// from 40 Hz to 16 kHz on a log scale, each 0-255.
namespace Spectrum {

constexpr int WindowSize = 1024;
constexpr int BandCount = 26;

// The most recent WindowSize mono samples, -1..1.
QByteArray bands(const QList<float> &samples, int sampleRate);

// Which band a frequency falls in.
int bandOf(double hz, int sampleRate);

} // namespace Spectrum
