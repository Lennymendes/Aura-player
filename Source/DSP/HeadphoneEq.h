// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Equalisers of the AURA DSP panel, RBJ biquads in 64 bits:
//   headphone profiles (up to 8 bands; the two Momentum M2 profiles are NX Audio's own) and studio curves,
//   and the 7-band parametric EQ edited by hand.
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
    const char* name;           // model ("Momentum M2")
    const char* maker;          // brand ("Sennheiser"), "" for studio curves
    const char* signature;      // second line ("Clear — ZEYN"), "" = none
    bool studioCurve;           // "Courbes studio" category (generic curves) instead of a headphone model
    int numBands;
    std::array<Band, 8> bands;
    juce::String displayName() const { return (juce::String::fromUTF8 (maker) + " " + juce::String::fromUTF8 (name)).trim(); }
    juce::String chipName() const { return juce::String::fromUTF8 (name).toUpperCase(); }
};

inline const std::array<Preset, 11>& presets()
{
    using T = BandType;
    static const std::array<Preset, 11> list {{
        { "momentum-m2", "Momentum M2", "Sennheiser", "", false, 7, {{
            { T::LowShelf, 105.0, 4.0, 0.7 }, { T::Peak, 66.0, -6.5, 0.5 }, { T::Peak, 220.0, -1.0, 0.8 }, { T::Peak, 6500.0, -1.5, 3.0 },
            { T::Peak, 4000.0, 1.5, 2.5 }, { T::Peak, 2700.0, -1.0, 2.8 }, { T::HighShelf, 10000.0, 3.0, 0.7 } }} },
        { "momentum-m2-cl", "Momentum M2 CL", "Sennheiser", "Clear — ZEYN", false, 7, {{
            { T::LowShelf, 105.0, 4.0, 0.7 }, { T::Peak, 66.0, -6.5, 0.5 }, { T::Peak, 220.0, -1.0, 0.8 }, { T::Peak, 6500.0, -1.5, 3.0 },
            { T::Peak, 4000.0, 1.5, 2.5 }, { T::Peak, 2700.0, -1.0, 2.8 }, { T::HighShelf, 10000.0, 1.0, 0.7 } }} },
        { "hd-650", "HD 650", "Sennheiser", "", false, 5, {{
            { T::LowShelf, 70.0, 3.5, 0.7 }, { T::Peak, 160.0, -2.0, 1.0 }, { T::Peak, 2100.0, 1.5, 1.4 }, { T::Peak, 4500.0, -2.5, 3.0 },
            { T::HighShelf, 10000.0, 2.0, 0.7 } }} },
        { "hd-600", "HD 600", "Sennheiser", "", false, 4, {{
            { T::LowShelf, 75.0, 4.0, 0.7 }, { T::Peak, 180.0, -1.5, 1.1 }, { T::Peak, 3300.0, -2.0, 2.5 }, { T::HighShelf, 10000.0, 1.5, 0.7 } }} },
        { "hd-800-s", "HD 800 S", "Sennheiser", "", false, 4, {{
            { T::LowShelf, 80.0, 4.5, 0.7 }, { T::Peak, 200.0, -1.5, 1.0 }, { T::Peak, 6000.0, -4.5, 4.0 }, { T::HighShelf, 10000.0, -2.0, 0.7 } }} },
        { "dt-1990-pro", "DT 1990 Pro", "Beyerdynamic", "", false, 3, {{
            { T::LowShelf, 80.0, 2.0, 0.7 }, { T::Peak, 200.0, -2.5, 1.0 }, { T::Peak, 8200.0, -5.5, 4.5 } }} },
        { "clear-mg", "Clear MG", "Focal", "", false, 3, {{
            { T::LowShelf, 80.0, 2.5, 0.7 }, { T::Peak, 1200.0, -1.5, 1.8 }, { T::HighShelf, 10000.0, 1.5, 0.7 } }} },
        { "em32", "EM32", "EarSonics", "", false, 3, {{
            { T::LowShelf, 60.0, 1.0, 0.7 }, { T::Peak, 3500.0, -1.5, 2.5 }, { T::HighShelf, 12000.0, 1.5, 0.7 } }} },
        { "warm", "Chaleur", "", "", true, 1, {{ { T::LowShelf, 105.0, 3.0, 0.7 } }} },
        { "clarity", "Clarté", "", "", true, 2, {{ { T::Peak, 3000.0, 1.5, 1.2 }, { T::HighShelf, 9000.0, 2.0, 0.7 } }} },
        { "night", "Écoute de nuit", "", "", true, 2, {{ { T::LowShelf, 120.0, -3.0, 0.7 }, { T::HighShelf, 8000.0, -2.0, 0.7 } }} },
    }};
    return list;
}

inline const Preset* find (const juce::String& id)
{
    for (auto& p : presets()) if (id == p.id) return &p;
    return nullptr;
}

/** Search: "hd650", "HD 650", "momentum m2", "focal"... (case, spaces and dashes ignored, every word must match). */
inline bool matches (const Preset& p, const juce::String& query)
{
    auto norm = [] (const juce::String& s) { return s.toLowerCase().removeCharacters (" -_.'"); };
    const auto hay = norm (juce::String::fromUTF8 (p.maker) + juce::String::fromUTF8 (p.name) + juce::String::fromUTF8 (p.signature));
    if (const auto q = norm (query); q.isEmpty() || hay.contains (q)) return true;
    for (auto& w : juce::StringArray::fromTokens (query, " ", {}))
        if (! hay.contains (norm (w))) return false;
    return true;
}

/** Parametric EQ (RME ADI-2 style): 7 bands — Bass (low shelf), Band 1-5 (peak; 1 and 5 may be shelves), Treble (high shelf). */
using ParametricBands = std::array<Band, 7>;
inline ParametricBands flatParametric()
{
    using T = BandType;
    return {{ { T::LowShelf, 100.0, 0.0, 0.7 }, { T::Peak, 200.0, 0.0, 1.0 }, { T::Peak, 500.0, 0.0, 1.0 }, { T::Peak, 1000.0, 0.0, 1.0 },
              { T::Peak, 3000.0, 0.0, 1.0 }, { T::Peak, 6000.0, 0.0, 1.0 }, { T::HighShelf, 10000.0, 0.0, 0.7 } }};
}

/** A headphone profile in the 7 slots: low shelf -> Bass, high shelf -> Treble, the others -> Band 1-5 in order. */
inline ParametricBands toParametric (const Preset& p)
{
    auto out = flatParametric();
    int next = 1;
    for (int i = 0; i < p.numBands; ++i)
    {
        const auto& b = p.bands[(size_t) i];
        if (b.type == BandType::LowShelf && out[0].gain == 0.0) out[0] = b;
        else if (b.type == BandType::HighShelf && out[6].gain == 0.0) out[6] = b;
        else if (next <= 5) out[(size_t) next++] = b;
    }
    return out;
}

inline juce::String typeName (BandType t) { return t == BandType::LowShelf ? "lowshelf" : t == BandType::HighShelf ? "highshelf" : "peak"; }
inline BandType typeFromName (const juce::String& s) { return s == "lowshelf" ? BandType::LowShelf : s == "highshelf" ? BandType::HighShelf : BandType::Peak; }

/** Clamped to the editor's ranges: gain ±12 dB (0.1), 20 Hz-20 kHz, Q 0.5-10. */
inline Band clampBand (Band b)
{
    b.gain = std::round (juce::jlimit (-12.0, 12.0, b.gain) * 10.0) / 10.0;
    b.freq = juce::jlimit (20.0, 20000.0, b.freq);
    b.q = juce::jlimit (0.5, 10.0, b.q);
    return b;
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

/** Magnitude of a band list at `freq` (dB). */
inline double responseDb (const Band* bands, int numBands, double fs, double freq)
{
    constexpr double pi = 3.14159265358979323846;
    const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * freq / fs), z2 = z1 * z1;
    double db = 0.0;
    for (int i = 0; i < numBands; ++i)
    {
        if (bands[i].gain == 0.0) continue;          // a 0 dB band is a pass-through
        const auto c = design (bands[i], fs);
        db += 20.0 * std::log10 (std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2)));
    }
    return db;
}

/** Magnitude of the whole preset at `freq` (dB, without the preamp). */
inline double responseDb (const Preset& p, double fs, double freq) { return responseDb (p.bands.data(), p.numBands, fs, freq); }

/** Largest boost of a curve between 20 Hz and 20 kHz (0 if it only cuts): the automatic headroom. */
template <typename Fn> double peakBoostOf (Fn&& curveDb)
{
    double peak = 0.0;
    for (int i = 0; i < 240; ++i) peak = std::max (peak, curveDb (20.0 * std::pow (1000.0, i / 239.0)));
    return peak;
}

inline double peakBoostDb (const Preset& p, double fs) { return peakBoostOf ([&] (double f) { return responseDb (p, fs, f); }); }

} // namespace nx::hpeq
