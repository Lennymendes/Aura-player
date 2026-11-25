// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Studio View: the live high-resolution spectrum (juce_dsp lives in its own file: its SIMD templates clash with the engine tests').
#include "TestFramework.h"
#include "../Source/Ext/AudioAnalyzer.h"
#include "../Source/DSP/HeadphoneEq.h"
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

NX_TEST_SECTION (128, "Profils casque : HD 650 et Momentum M2 alignés sur les courbes de référence de l'éditeur EQ (à 0,3 dB près)")
{
    // Points read on the reference curves (EQ editor screenshots, "HD650" and "MOMENTUM M2" presets): Hz -> dB.
    struct P { double hz, db; };
    const P m2[] = { { 10, -2.35 }, { 20, -1.97 }, { 30, -1.42 }, { 60, -0.11 }, { 100, -0.11 }, { 150, -0.98 }, { 200, -1.31 }, { 500, -0.38 }, { 1000, 0.05 }, { 2000, 0.87 },
                     { 3000, -1.26 }, { 4000, -0.22 }, { 6000, -2.02 }, { 8000, -1.26 }, { 10000, -0.66 }, { 20000, -0.05 } };
    const P hd[] = { { 10, 5.46 }, { 30, 4.86 }, { 60, 2.90 }, { 100, -0.98 }, { 150, -4.21 }, { 200, -5.03 }, { 500, -2.51 }, { 1000, -2.51 }, { 2000, -1.04 }, { 3000, -3.11 },
                     { 5000, -1.20 }, { 8000, 0.27 }, { 10000, 0.38 }, { 15000, -2.79 }, { 20000, -4.21 } };
    auto worst = [] (const char* id, const P* pts, size_t n)
    {
        const auto* p = nx::hpeq::find (id);
        double w = 99.0;
        if (p != nullptr)
        {
            w = 0.0;
            for (size_t i = 0; i < n; ++i) w = std::max (w, std::abs (nx::hpeq::responseDb (*p, 192000.0, pts[i].hz) - pts[i].db));   // the editor draws the analogue curve
        }
        return w;
    };
    const double wm = worst ("momentum-m2", m2, sizeof (m2) / sizeof (P)), wh = worst ("hd-650", hd, sizeof (hd) / sizeof (P));
    t.check (wm < 0.3 && wh < 0.3, "écart maximal à la courbe de référence : Momentum M2 "_t + juce::String (wm, 2) + " dB, HD 650 " + juce::String (wh, 2) + " dB");
}
