// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// E9DS: transparent de-esser / high-frequency softener of AURA DSP, in 64 bits, before the final quantisation and
// before the DSD modulator (it lives in both DSP chains, like the studio tools).
//   Detector   linked (all channels summed), band-pass at Focus (Split-Band) or 2nd-order high-pass (Dynamic HF);
//              mean power of the band (5 ms) and of the full signal (1 ms: the band never leads at an onset, so the
//              click of music starting from silence is not taken for treble); the threshold is automatic: the band's share of
//              the signal, r = band / full, above -6 dB (6 dB soft knee, 4:1); the reduction moves with an attack of
//              1 ms and a release of 60 ms.
//   Split-Band dynamic bell, minimum phase: y = x - (1 - g) * bandpass(x) (constant 0 dB peak, Q 2): g at Focus,
//              nothing far from it. No linear phase: no pre-ringing on transients, little CPU at 352.8 kHz.
//   Dynamic HF gentle shelf above Focus: y = lowpass1(x) + g * highpass1(x) = x - (1 - g) * highpass1(x).
// Below the threshold g is exactly 1 and the output is bit-identical to the input. Reduction capped at Amount
// (0 to -6 dB). The switch ramps the effect in / out over 20 ms (no click), then the processor bypasses itself.
// prepare() allocates nothing either (fixed arrays); process() is real-time safe.
#pragma once

#include "../Engine/DspChain.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

namespace nx
{

struct E9dsState
{
    enum Mode { SplitBand = 0, DynamicHf = 1 };
    static constexpr double minFocus = 4000.0, maxFocus = 12000.0, maxAmountDb = 6.0;
    std::atomic<bool> enabled { false };
    std::atomic<double> focusHz { 6500.0 };
    std::atomic<double> amountDb { 6.0 };            // reduction cap, dB (shown as -amount)
    std::atomic<int> mode { SplitBand };
    std::atomic<double> reductionDb { 0.0 };         // meter: largest reduction of the last block (audio thread)

    void set (bool on, double focus, double amount, int m)
    {
        focusHz = std::clamp (focus, minFocus, maxFocus);
        amountDb = std::clamp (std::abs (amount), 0.0, maxAmountDb);
        mode = m == DynamicHf ? DynamicHf : SplitBand;
        enabled = on;
    }
};

class E9dsProcessor : public DspProcessor
{
public:
    static constexpr int maxChannels = 8;
    explicit E9dsProcessor (std::shared_ptr<E9dsState> s) : state (std::move (s)) {}

    juce::String getName() const override
    {
        const juce::String f = juce::String (state->focusHz.load() / 1000.0, 1).replaceCharacter ('.', ',');
        return "E9DS " + juce::String (state->mode.load() == E9dsState::DynamicHf ? "Dynamic HF " : "Split-Band ") + f + " kHz";
    }
    juce::String getDescription() const override { return "De-esser / adoucisseur d'aigus transparent (64 bits)"; }
    bool isBypassed() const noexcept override { return ! state->enabled.load() && ! audible.load (std::memory_order_relaxed); }

    bool prepare (double sampleRate, int, int) override
    {
        fs = sampleRate;
        average = 1.0 - std::exp (-1.0 / (0.005 * fs));
        averageFull = 1.0 - std::exp (-1.0 / (0.001 * fs));
        attack = 1.0 - std::exp (-1.0 / (0.001 * fs));
        release = 1.0 - std::exp (-1.0 / (0.060 * fs));
        mixStep = 1.0 / (0.020 * fs);
        designedFocus = -1.0;
        reset();
        return true;
    }

    void reset() noexcept override
    {
        det = {}; app = {}; hp1 = {};
        eBand = eFull = grSmooth = 0.0;
        mix = 0.0;
        audible.store (false, std::memory_order_relaxed);
    }

    void process (double* const* ch, int numChannels, int numFrames) noexcept override
    {
        const int nc = std::min (numChannels, maxChannels);
        if (! audible.load (std::memory_order_relaxed)) { det = {}; app = {}; hp1 = {}; eBand = eFull = grSmooth = 0.0; mix = 0.0; }   // waking up
        const double focus = state->focusHz.load();
        const int m = state->mode.load();
        if (focus != designedFocus || m != designedMode) design (focus, m);
        const double target = state->enabled.load() ? 1.0 : 0.0;
        const double capDb = state->amountDb.load();
        const double inv = 1.0 / nc;
        double worst = 0.0;
        for (int i = 0; i < numFrames; ++i)
        {
            // detector (linked)
            double s = 0.0;
            for (int c = 0; c < nc; ++c) s += ch[c][i];
            s *= inv;
            const double b = det.run (s, dc);
            eFull += averageFull * (s * s - eFull);
            eBand += average * (b * b - eBand);
            double gr = 0.0;                                         // dB of reduction wanted
            if (eBand > kneeStart * eFull && eBand > 1e-20)
            {
                const double over = 10.0 * std::log10 (eBand / eFull) - thresholdDb;
                gr = over < knee / 2.0 ? slope * (over + knee / 2.0) * (over + knee / 2.0) / (2.0 * knee) : slope * over;
                gr = std::min (gr, capDb);
            }
            grSmooth += (gr > grSmooth ? attack : release) * (gr - grSmooth);
            if (gr == 0.0 && grSmooth < 1e-4) grSmooth = 0.0;         // released: exactly 0 again (bit-identical)
            worst = std::max (worst, grSmooth);
            const double cut = grSmooth > 0.0 ? 1.0 - std::pow (10.0, -grSmooth / 20.0) : 0.0;   // 1 - g

            if (mix != target) mix = target > mix ? std::min (target, mix + mixStep) : std::max (target, mix - mixStep);
            const double k = cut * mix;
            for (int c = 0; c < nc; ++c)
            {
                const double x = ch[c][i];
                const double f = m == E9dsState::DynamicHf ? hp1[(size_t) c].run (x, h1) : app[(size_t) c].run (x, ac);
                ch[c][i] = x - k * f;                                // k = 0: bit-identical
            }
        }
        audible.store (mix > 0.0, std::memory_order_relaxed);
        // meter: written by whichever copy runs (audio thread for PCM out, decoder before the DSD modulator: never both)
        state->reductionDb.store (worst * (target > 0.0 ? 1.0 : 0.0), std::memory_order_relaxed);
    }

private:
    struct Coef { double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct Biquad
    {
        double z1 = 0, z2 = 0;
        double run (double x, const Coef& k) noexcept
        {
            const double y = k.b0 * x + z1;
            z1 = k.b1 * x - k.a1 * y + z2;
            z2 = k.b2 * x - k.a2 * y;
            return y;
        }
    };
    struct OnePoleHp
    {
        double x1 = 0, y1 = 0;
        double run (double x, const Coef& k) noexcept { const double y = k.b0 * (x - x1) - k.a1 * y1; x1 = x; y1 = y; return y; }
    };

    void design (double focus, int m) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const double w = 2.0 * pi * std::min (focus, 0.45 * fs) / fs, cw = std::cos (w), sw = std::sin (w);
        auto bandpass = [&] (double q)                               // RBJ, constant 0 dB peak
        {
            const double a = sw / (2.0 * q), a0 = 1.0 + a;
            return Coef { a / a0, 0.0, -a / a0, -2.0 * cw / a0, (1.0 - a) / a0 };
        };
        ac = bandpass (2.0);
        if (m == E9dsState::DynamicHf)
        {
            const double a = sw / (2.0 * 0.70710678118654752), a0 = 1.0 + a;   // RBJ high-pass, Butterworth
            dc = { (1.0 + cw) / 2.0 / a0, -(1.0 + cw) / a0, (1.0 + cw) / 2.0 / a0, -2.0 * cw / a0, (1.0 - a) / a0 };
        }
        else dc = ac;
        const double t = std::tan (w / 2.0);                         // bilinear first-order high-pass
        h1 = { 1.0 / (1.0 + t), 0.0, 0.0, (t - 1.0) / (1.0 + t), 0.0 };
        designedFocus = focus;
        designedMode = m;
    }

    static constexpr double thresholdDb = -6.0, knee = 6.0, slope = 1.0 - 1.0 / 4.0;
    static inline const double kneeStart = std::pow (10.0, (thresholdDb - knee / 2.0) / 10.0);

    std::shared_ptr<E9dsState> state;
    double fs = 48000.0, average = 0.0, averageFull = 0.0, attack = 0.0, release = 0.0, mixStep = 0.0;
    double designedFocus = -1.0;
    int designedMode = -1;
    Coef dc, ac, h1;
    Biquad det;
    std::array<Biquad, maxChannels> app;
    std::array<OnePoleHp, maxChannels> hp1;
    double eBand = 0.0, eFull = 0.0, grSmooth = 0.0, mix = 0.0;
    std::atomic<bool> audible { false };
};

} // namespace nx
