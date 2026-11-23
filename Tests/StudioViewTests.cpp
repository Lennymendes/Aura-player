// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Studio View: the live high-resolution spectrum (juce_dsp lives in its own file: its SIMD templates clash with the engine tests').
#include "TestFramework.h"
#include "../Source/Ext/AudioAnalyzer.h"
#include <cmath>

extern thread_local bool nxtest_countAllocations;
extern std::atomic<long> nxtest_allocationCount;

using namespace nxtest;

NX_TEST_SECTION (127, "Studio View : spectre en direct haute résolution (jusqu'à Nyquist), 0 allocation côté thread audio")
{
    constexpr double pi = juce::MathConstants<double>::pi;
    // 2. live spectrum (Studio View): a 30 kHz tone at 96 kHz lands at 30 kHz; the tap the audio thread writes allocates nothing
    {
        nx::AnalyzerTap tap;
        tap.rate = 96000.0;
        std::vector<float> st (4096 * 2);
        for (int i = 0; i < 4096; ++i) st[(size_t) i * 2] = st[(size_t) i * 2 + 1] = 0.5f * (float) std::sin (2.0 * pi * 30000.0 * i / 96000.0);
        nxtest_allocationCount = 0;
        nxtest_countAllocations = true;
        for (int k = 0; k < 4; ++k) tap.push (st.data() + (size_t) k * 2048, 1024);
        nxtest_countAllocations = false;
        const long allocs = nxtest_allocationCount.load();
        nx::AudioAnalyzer an (tap);
        an.update (0.033, true);
        const auto& fine = an.getFineSpectrum();
        int best = 1;
        for (int i = 1; i < (int) fine.size(); ++i) if (fine[(size_t) i] > fine[(size_t) best]) best = i;
        const double hz = best * 96000.0 / nx::AudioAnalyzer::fftSize;
        t.check ((int) fine.size() == nx::AudioAnalyzer::fftSize / 2 && std::abs (hz - 30000.0) < 50.0 && fine[(size_t) best] > -12.0 && allocs == 0,
                 "spectre haute résolution jusqu'à 48 kHz : raie à "_t + juce::String (hz, 0) + " Hz (" + juce::String (fine[(size_t) best], 1)
                     + " dB), 0 allocation côté thread audio"_t);
    }
}
