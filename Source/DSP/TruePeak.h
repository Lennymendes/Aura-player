// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// True Peak (dBTP), ITU-R BS.1770-4 annex 2: 4x oversampling with the standard's 48-tap polyphase FIR
// (4 phases x 12 taps), the peak of the absolute values over all channels (the stored samples included).
// Auto-Headroom: a track whose True Peak exceeds the ceiling is attenuated by exactly the excess, a clean track by 0 dB.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace nx::truepeak
{

/** dB of attenuation for a track (0 when unknown, NaN, or already under the ceiling). */
inline double attenuationDb (double truePeakDb, double ceilingDb) noexcept
{
    return std::isfinite (truePeakDb) && truePeakDb > ceilingDb ? truePeakDb - ceilingDb : 0.0;
}

/** Linear gain of the Auto-Headroom (1 when off / unknown / clean). */
inline double gain (bool on, double truePeakDb, double ceilingDb) noexcept
{
    const double a = on ? attenuationDb (truePeakDb, ceilingDb) : 0.0;
    return a > 0.0 ? std::pow (10.0, -a / 20.0) : 1.0;
}

class Meter
{
public:
    explicit Meter (int numChannels) : ch (std::max (1, numChannels)), hist ((size_t) ch * taps, 0.0) {}

    /** Interleaved samples (full scale = 1). */
    void add (const double* x, int frames) noexcept
    {
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c)
            {
                double* h = hist.data() + (size_t) c * taps;
                std::move (h + 1, h + taps, h);              // h[taps-1] = newest
                const double v = x[(size_t) i * (size_t) ch + (size_t) c];
                h[taps - 1] = v;
                peak = std::max (peak, std::abs (v));
                for (int p = 0; p < 4; ++p)
                {
                    double y = 0.0;
                    for (int k = 0; k < taps; ++k) y += coef[(size_t) p][(size_t) k] * h[taps - 1 - k];
                    peak = std::max (peak, std::abs (y));
                }
            }
    }
    double linear() const noexcept { return peak; }
    double dbtp() const noexcept { return peak > 0.0 ? 20.0 * std::log10 (peak) : -200.0; }

private:
    static constexpr int taps = 12;
    static constexpr std::array<std::array<double, taps>, 4> coef { {
        { 0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
          0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500 },
        { -0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
          0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375 },
        { -0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
          0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875 },
        { -0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
          0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750 } } };
    int ch;
    std::vector<double> hist;
    double peak = 0.0;
};

} // namespace nx::truepeak
