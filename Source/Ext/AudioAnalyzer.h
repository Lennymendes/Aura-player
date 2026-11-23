// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Message-thread analysis of what is playing (from the engine's lock-free tap): 64-band log
// spectrum, VU / peak ballistics, oscilloscope. Feeds the built-in meters and the JS plug-ins.
#pragma once

#include "../Engine/PlaybackEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace nx
{

class AudioAnalyzer
{
public:
    static constexpr int numBands = 64, scopeSize = 512, fftOrder = 12;

    explicit AudioAnalyzer (AnalyzerTap& tap);

    /** Pulls new samples and updates every value. Call once per UI frame. */
    void update (double dtSeconds, bool playing);

    const std::array<float, numBands>& getSpectrum() const noexcept { return bands; }   // 0..1 (−90..0 dB)
    float getRms (int ch) const noexcept  { return rms[(size_t) (ch & 1)]; }            // 0..1 linear, VU ballistics
    float getPeak (int ch) const noexcept { return peak[(size_t) (ch & 1)]; }
    const std::array<float, scopeSize>& getScope() const noexcept { return scope; }
    /** Studio View: full-resolution spectrum, dB per FFT bin (0 .. Nyquist of the stream, n / 2 bins), smoothed. */
    const std::vector<float>& getFineSpectrum() const noexcept { return fine; }
    static constexpr int fftSize = 1 << fftOrder;
    double getSampleRate() const noexcept { return rate; }
    bool isDsdDensity() const noexcept { return dsd; }
    float getLevel() const noexcept { return 0.5f * (rms[0] + rms[1]); }

    /** For the JS API: { spectrum: [...], rmsL, rmsR, peakL, peakR, scope: [...], rate, dsd } */
    juce::var toVar() const;

private:
    AnalyzerTap& tap;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> history, window, fftData, pullBuffer, fine;
    int historyPos = 0;
    std::array<float, numBands> bands {};
    std::array<float, scopeSize> scope {};
    std::array<float, 2> rms {}, peak {}, rmsAcc {};
    double rate = 44100.0;
    bool dsd = false;
};

} // namespace nx
