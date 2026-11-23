// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// "Faux Hi-Res" detector (Studio View): from the long-term averaged power spectrum of a PCM file at >= 88,2 kHz.
//   wall = level just under 22,05 kHz (20-21,5 kHz) against just above (22,5-24 kHz), and the same around 24 kHz: a CD
//          (or 48 kHz) master upsampled to 96 kHz has a brick-wall cut there (its anti-alias filter), whatever floor
//          (dither / noise) sits above;
//   ext  = level of 25-40 kHz against the top of the audible band (16-20 kHz): a real high-resolution recording keeps
//          harmonics / air above 22 kHz, an upsampled 48 kHz master has nothing past 24 kHz.
// Fake: wall >= 20 dB or ext < -60 dB.  Real: wall < 10 dB and ext >= -40 dB.  Otherwise doubtful.
#pragma once

#include <juce_core/juce_core.h>
#include "../Core/Text.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nx::hires
{

enum Verdict { NotApplicable = 0, Real = 1, Doubtful = 2, Fake = 3, Undetermined = 4 };

struct Check
{
    int verdict = NotApplicable;
    double extDb = -200.0, wallDb = 0.0;
};

/** psd: mean power per FFT bin (0 .. n/2), binHz = fs / n. */
inline Check evaluate (const std::vector<double>& psd, double binHz, double fs)
{
    if (fs < 88199.0 || psd.size() < 16 || binHz <= 0.0) return {};
    auto band = [&] (double f0, double f1)
    {
        const int a = (int) std::ceil (f0 / binHz), b = std::min ((int) std::floor (f1 / binHz), (int) psd.size() - 1);
        if (b < a) return 0.0;
        double s = 0.0;
        for (int i = a; i <= b; ++i) s += psd[(size_t) i];
        return s / (double) (b - a + 1);
    };
    const double ref = band (16000.0, 20000.0), audible = band (20.0, 20000.0);
    const double below = band (20000.0, 21500.0), above = band (22500.0, 24000.0), ext = band (25000.0, std::min (40000.0, 0.45 * fs));
    const double below24 = band (22500.0, 23800.0), above24 = band (24300.0, 26000.0);   // 48 kHz master: its wall is at 24 kHz
    Check c;
    if (ref <= 1e-24 || ref < audible * 1e-9) { c.verdict = Undetermined; return c; }   // nothing at the top of the audible band
    c.wallDb = std::max (10.0 * std::log10 (std::max (below, 1e-30) / std::max (above, 1e-30)),
                         10.0 * std::log10 (std::max (below24, 1e-30) / std::max (above24, 1e-30)));
    c.extDb = 10.0 * std::log10 (std::max (ext, 1e-30) / ref);
    c.verdict = c.wallDb >= 20.0 || c.extDb < -60.0 ? Fake : c.wallDb < 10.0 && c.extDb >= -40.0 ? Real : Doubtful;
    return c;
}

inline juce::String verdictName (int v)
{
    switch (v)
    {
        case Real:         return "Vrai Hi-Res"_t;
        case Fake:         return "Faux Hi-Res probable"_t;
        case Doubtful:     return "Hi-Res douteux"_t;
        case Undetermined: return "Indéterminé (pas assez d'aigus)"_t;
        default:           break;
    }
    return "Non applicable (moins de 88,2 kHz)"_t;
}

/** One line: verdict and the two measures. */
inline juce::String describe (const Check& c)
{
    if (c.verdict == NotApplicable || c.verdict == Undetermined) return verdictName (c.verdict);
    const auto num = [] (double v) { return juce::String (v, 1).replaceCharacter ('.', ','); };
    juce::String why = c.verdict == Fake ? (c.wallDb >= 20.0 ? "coupure nette vers 22-24 kHz ("_t + num (-c.wallDb) + " dB), source 44,1 / 48 kHz suréchantillonnée ?"_t
                                                             : "rien au-delà de 24 kHz ("_t + num (c.extDb) + " dB)")
                 : "énergie 25-40 kHz : "_t + num (c.extDb) + " dB, marche à 22-24 kHz : "_t + num (-c.wallDb) + " dB";
    return verdictName (c.verdict) + " : " + why;
}

} // namespace nx::hires
