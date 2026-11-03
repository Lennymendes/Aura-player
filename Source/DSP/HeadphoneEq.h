// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Headphone equaliser (MUSE panel): presets of up to 8 bands (low shelf, peak, high shelf), RBJ biquads in 64 bits.
// The two Sennheiser MOMENTUM M2 profiles are the ones measured for NX Audio; the others are gentle generic curves.
// Headroom management: the preamp is the peak boost of the whole curve (measured on 240 log-spaced points), so a
// full-scale track never clips.
#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <juce_core/juce_core.h>

namespace nx::hpeq
{

enum class BandType { LowShelf, Peak, HighShelf };
struct Band { BandType type; double freq, gain, q; };

struct Preset
{
    const char* id;
    const char* name;
    const char* maker;          // "Sennheiser", "" for generic curves
    bool overEar;               // icon: over-ear (true) or generic curve
    int numBands;
    std::array<Band, 8> bands;
};

inline const std::array<Preset, 5>& presets()
{
    using T = BandType;
    static const std::array<Preset, 5> list {{
        { "momentum-m2", "MOMENTUM M2", "Sennheiser", true, 7, {{
            { T::LowShelf, 105.0, 4.0, 0.7 }, { T::Peak, 66.0, -6.5, 0.5 }, { T::Peak, 220.0, -1.0, 0.8 }, { T::Peak, 6500.0, -1.5, 3.0 },
            { T::Peak, 4000.0, 1.5, 2.5 }, { T::Peak, 2700.0, -1.0, 2.8 }, { T::HighShelf, 10000.0, 3.0, 0.7 } }} },
        { "momentum-m2-cl", "MOMENTUM M2 CL", "Sennheiser", true, 7, {{
            { T::LowShelf, 105.0, 4.0, 0.7 }, { T::Peak, 66.0, -6.5, 0.5 }, { T::Peak, 220.0, -1.0, 0.8 }, { T::Peak, 6500.0, -1.5, 3.0 },
            { T::Peak, 4000.0, 1.5, 2.5 }, { T::Peak, 2700.0, -1.0, 2.8 }, { T::HighShelf, 10000.0, 1.0, 0.7 } }} },
        { "warm", "Chaleur", "", false, 1, {{ { T::LowShelf, 105.0, 3.0, 0.7 } }} },
        { "clarity", "Clarté", "", false, 2, {{ { T::Peak, 3000.0, 1.5, 1.2 }, { T::HighShelf, 9000.0, 2.0, 0.7 } }} },
        { "night", "Écoute de nuit", "", false, 2, {{ { T::LowShelf, 120.0, -3.0, 0.7 }, { T::HighShelf, 8000.0, -2.0, 0.7 } }} },
    }};
    return list;
}

inline const Preset* find (const juce::String& id)
{
    for (auto& p : presets()) if (id == p.id) return &p;
    return nullptr;
}

/** Normalised biquad (a0 = 1). */
struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

inline Coeffs design (const Band& b, double fs)
{
    constexpr double pi = 3.14159265358979323846;
    const double f = juce::jlimit (10.0, fs * 0.45, b.freq);
    const double A = std::pow (10.0, b.gain / 40.0), w0 = 2.0 * pi * f / fs, cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * b.q), sA = 2.0 * std::sqrt (A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    switch (b.type)
    {
        case BandType::LowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sA);
            a0 = (A + 1) + (A - 1) * cw + sA;       a1 = -2 * ((A - 1) + (A + 1) * cw);     a2 = (A + 1) + (A - 1) * cw - sA;
            break;
        case BandType::HighShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sA);
            a0 = (A + 1) - (A - 1) * cw + sA;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sA;
            break;
        case BandType::Peak:
        default:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
    }
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

/** Magnitude of the whole preset at `freq` (dB, without the preamp). */
inline double responseDb (const Preset& p, double fs, double freq)
{
    constexpr double pi = 3.14159265358979323846;
    const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * freq / fs), z2 = z1 * z1;
    double db = 0.0;
    for (int i = 0; i < p.numBands; ++i)
    {
        const auto c = design (p.bands[(size_t) i], fs);
        db += 20.0 * std::log10 (std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2)));
    }
    return db;
}

/** Largest boost of the curve between 20 Hz and 20 kHz (0 if it only cuts): the automatic headroom. */
inline double peakBoostDb (const Preset& p, double fs)
{
    double peak = 0.0;
    for (int i = 0; i < 240; ++i)
        peak = std::max (peak, responseDb (p, fs, 20.0 * std::pow (1000.0, i / 239.0)));
    return peak;
}

} // namespace nx::hpeq
