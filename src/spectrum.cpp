#include "spectrum.h"

#include <cmath>
#include <complex>
#include <vector>

namespace Spectrum {

namespace {

constexpr double lowestHz = 40.0;
constexpr double highestHz = 16000.0;
// Levels below this many decibels under full scale show as an empty bar.
constexpr double floorDb = -60.0;

double edge(int band) {
    return lowestHz * std::pow(highestHz / lowestHz, double(band) / BandCount);
}

void fft(std::vector<std::complex<double>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2 * M_PI / double(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

} // namespace

int bandOf(double hz, int) {
    if (hz <= lowestHz)
        return 0;
    const int band = int(std::floor(BandCount * std::log(hz / lowestHz) / std::log(highestHz / lowestHz)));
    return qBound(0, band, BandCount - 1);
}

QByteArray bands(const QList<float> &samples, int sampleRate) {
    const int n = WindowSize;
    std::vector<std::complex<double>> data(n);
    const int offset = qMax(0, int(samples.size()) - n);
    for (int i = 0; i < n; ++i) {
        const double sample = offset + i < samples.size() ? samples[offset + i] : 0.0;
        const double hann = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        data[i] = sample * hann;
    }
    fft(data);

    // A full-scale sine under a Hann window peaks at n / 4.
    const double fullScale = n / 4.0;
    const double binHz = double(sampleRate) / n;
    QByteArray out(BandCount, '\0');
    for (int band = 0; band < BandCount; ++band) {
        int first = int(std::ceil(edge(band) / binHz));
        int last = int(std::floor(edge(band + 1) / binHz));
        if (last < first) // low bands are narrower than one bin
            first = last = int(std::round((edge(band) + edge(band + 1)) / 2 / binHz));
        double peak = 0;
        for (int bin = qMax(1, first); bin <= qMin(last, n / 2 - 1); ++bin)
            peak = std::max(peak, std::abs(data[bin]));
        if (peak <= 0)
            continue;
        const double db = 20 * std::log10(peak / fullScale);
        const double level = (db - floorDb) / -floorDb;
        out[band] = static_cast<char>(qBound(0, int(std::lround(level * 255)), 255));
    }
    return out;
}

} // namespace Spectrum
