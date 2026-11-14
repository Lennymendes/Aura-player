// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Studio tools of the player bar: listening checks used in mastering, applied in 64 bits after the decoder.
//   Mono (L+R)      both channels carry (L + R) / 2
//   Solo Mid / Side M = (L + R) / 2 or S = (L − R) / 2 on both channels
//   Phase (Ø)       polarity inversion of both channels (absolute phase)
//   Smart crossfeed Bauer stereophonic-to-binaural crossfeed (bs2b, 700 Hz / 4.5 dB), off by itself in mono
//   Headphone EQ    profile of HeadphoneEq.h (Momentum M2...), bypass switch
//   Parametric EQ   7 bands edited by hand, in series after the headphone EQ; one preamp cancels the peak of the
//                   combined curve (automatic headroom: no clipping)
// Any active tool takes the stream out of bit-perfect (the Signal Path says so); all off = bypassed.
// The switches are atomics read by the audio thread: toggling never rebuilds the chain.
#pragma once

#include "../Engine/DspChain.h"
#include "HeadphoneEq.h"
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

namespace nx
{

struct StudioToolsState
{
    enum class Monitor { Stereo, Mono, MidSolo, SideSolo };
    std::atomic<Monitor> monitor { Monitor::Stereo };
    std::atomic<bool> invertPhase { false };
    std::atomic<bool> crossfeed { false };
    // headphone EQ: preset index in hpeq::presets() (-1 = none), bypass keeps the choice, preamp from the headroom
    std::atomic<int> eqPreset { -1 };
    std::atomic<bool> eqBypass { false };
    std::atomic<bool> autoHeadroom { true };
    std::atomic<double> eqPreampDb { 0.0 };
    std::atomic<int> eqVersion { 0 };
    // parametric EQ (7 bands, edited by hand): the parameters are atomics published with a sequence counter (odd while
    // the message thread writes them), so the audio thread never reads a half-written band
    struct AtomicBand { std::atomic<int> type { 0 }; std::atomic<double> freq { 1000.0 }, gain { 0.0 }, q { 1.0 }; };
    std::array<AtomicBand, 7> peq;
    std::atomic<bool> peqEnabled { false };
    std::atomic<int> peqSequence { 0 };

    StudioToolsState() { writeBands (hpeq::flatParametric()); }

    bool eqActive() const noexcept { return eqPreset.load() >= 0 && ! eqBypass.load(); }
    bool peqActive() const noexcept { return peqEnabled.load(); }
    bool anyActive() const noexcept { return monitor.load() != Monitor::Stereo || invertPhase.load() || crossfeed.load() || eqActive() || peqActive(); }
    const hpeq::Preset* eqPresetInfo() const noexcept
    {
        const int i = eqPreset.load();
        return i >= 0 && i < (int) hpeq::presets().size() ? &hpeq::presets()[(size_t) i] : nullptr;
    }
    /** Message thread: chooses a headphone profile (-1 = off); the audio thread picks it up. */
    void setEq (int index, bool bypass)
    {
        eqPreset = index >= 0 && index < (int) hpeq::presets().size() ? index : -1;
        eqBypass = bypass;
        updateHeadroom();
    }
    /** Message thread: the 7 bands of the parametric EQ (clamped to the editor's ranges) and its switch. */
    void setParametric (const hpeq::ParametricBands& bands, bool enabled)
    {
        hpeq::ParametricBands b;
        for (size_t i = 0; i < b.size(); ++i) b[i] = hpeq::clampBand (bands[i]);
        writeBands (b);
        peqEnabled = enabled;
        updateHeadroom();
    }
    hpeq::ParametricBands parametricBands() const
    {
        hpeq::ParametricBands b;
        for (size_t i = 0; i < b.size(); ++i) b[i] = { (hpeq::BandType) peq[i].type.load(), peq[i].freq.load(), peq[i].gain.load(), peq[i].q.load() };
        return b;
    }
    /** The combined curve (headphone profile then parametric EQ, the ones that are on), without the preamp. */
    double combinedDb (double fs, double freq) const
    {
        double db = 0.0;
        if (const auto* p = eqPresetInfo(); p != nullptr && eqActive()) db += hpeq::responseDb (*p, fs, freq);
        if (peqActive()) { const auto b = parametricBands(); db += hpeq::responseDb (b.data(), (int) b.size(), fs, freq); }
        return db;
    }
    /** Headroom management: the preamp cancels the peak of the combined curve, so nothing ever clips. */
    void updateHeadroom()
    {
        const double peak = hpeq::peakBoostOf ([this] (double f) { return combinedDb (48000.0, f); });
        eqPreampDb = autoHeadroom.load() && (eqActive() || peqActive()) && peak > 0.0 ? -(peak + 0.1) : 0.0;
        ++eqVersion;
    }

private:
    void writeBands (const hpeq::ParametricBands& b)
    {
        peqSequence.fetch_add (1);                       // odd: being written
        for (size_t i = 0; i < b.size(); ++i)
        {
            peq[i].type = (int) b[i].type; peq[i].freq = b[i].freq; peq[i].gain = b[i].gain; peq[i].q = b[i].q;
        }
        peqSequence.fetch_add (1);                       // even: complete
    }
public:
    juce::String describe() const;
};

class StudioToolsProcessor : public DspProcessor
{
public:
    explicit StudioToolsProcessor (std::shared_ptr<StudioToolsState> s) : state (std::move (s)) {}

    juce::String getName() const override { return "Outils Studio : " + state->describe(); }
    juce::String getDescription() const override { return "Mono, solo Mid / Side, phase, crossfeed, égaliseurs (64 bits)"; }
    bool isBypassed() const noexcept override { return ! state->anyActive(); }

    bool prepare (double sampleRate, int, int) override
    {
        fs = sampleRate;
        seenEqVersion = -1;
        seenPeq = -1;
        // bs2b default level: 700 Hz, 4.5 dB of feed
        constexpr double pi = 3.14159265358979323846;
        const double fcLo = 700.0, feed = 4.5;
        const double gbLo = feed * -5.0 / 6.0 - 3.0, gbHi = feed / 6.0 - 3.0;
        const double gLo = std::pow (10.0, gbLo / 20.0);
        const double gHi = 1.0 - std::pow (10.0, gbHi / 20.0);
        const double fcHi = fcLo * std::pow (2.0, (gbLo - 20.0 * std::log10 (gHi)) / 12.0);
        double x = std::exp (-2.0 * pi * fcLo / sampleRate);
        b1Lo = x; a0Lo = gLo * (1.0 - x);
        x = std::exp (-2.0 * pi * fcHi / sampleRate);
        b1Hi = x; a0Hi = 1.0 - gHi * (1.0 - x); a1Hi = -x;
        gain = 1.0 / (1.0 - gHi + gLo);
        reset();
        return true;
    }

    void reset() noexcept override { lo = {}; hi = {}; asis = {}; eqZ = {}; peqZ = {}; }

    void process (double* const* ch, int numChannels, int numFrames) noexcept override
    {
        if (state->eqActive() || state->peqActive()) applyEq (ch, juce::jmin (numChannels, 2), numFrames);
        if (numChannels < 2) { if (state->invertPhase.load()) for (int i = 0; i < numFrames; ++i) ch[0][i] = -ch[0][i]; return; }
        const auto monitor = state->monitor.load();
        const bool invert = state->invertPhase.load();
        const bool xfeed = state->crossfeed.load() && monitor == StudioToolsState::Monitor::Stereo;
        double* l = ch[0];
        double* r = ch[1];
        for (int i = 0; i < numFrames; ++i)
        {
            double L = l[i], R = r[i];
            switch (monitor)
            {
                case StudioToolsState::Monitor::Mono:
                case StudioToolsState::Monitor::MidSolo:  L = R = 0.5 * (L + R); break;
                case StudioToolsState::Monitor::SideSolo: L = R = 0.5 * (L - R); break;
                case StudioToolsState::Monitor::Stereo:   break;
            }
            if (xfeed)
            {
                lo[0] = a0Lo * L + b1Lo * lo[0];
                lo[1] = a0Lo * R + b1Lo * lo[1];
                hi[0] = a0Hi * L + a1Hi * asis[0] + b1Hi * hi[0];
                hi[1] = a0Hi * R + a1Hi * asis[1] + b1Hi * hi[1];
                asis[0] = L; asis[1] = R;
                L = (hi[0] + lo[1]) * gain;
                R = (hi[1] + lo[0]) * gain;
            }
            if (invert) { L = -L; R = -R; }
            l[i] = L;
            r[i] = R;
        }
        // extra channels (multichannel): polarity only
        if (invert)
            for (int c = 2; c < numChannels; ++c)
                for (int i = 0; i < numFrames; ++i) ch[c][i] = -ch[c][i];
    }

private:
    /** Headphone profile then parametric EQ, in series, after one preamp (the headroom). No allocation, no lock. */
    void applyEq (double* const* ch, int numChannels, int numFrames) noexcept
    {
        if (const int v = state->eqVersion.load(); v != seenEqVersion)
        {
            seenEqVersion = v;
            const auto* p = state->eqPresetInfo();
            numEq = p != nullptr ? p->numBands : 0;
            for (int b = 0; b < numEq; ++b) eqC[(size_t) b] = hpeq::design (p->bands[(size_t) b], fs);
            preamp = std::pow (10.0, state->eqPreampDb.load() / 20.0);
            eqZ = {};
        }
        if (const int seq = state->peqSequence.load (std::memory_order_acquire); seq != seenPeq && (seq & 1) == 0)
        {
            hpeq::ParametricBands b;
            for (size_t i = 0; i < b.size(); ++i)
                b[i] = { (hpeq::BandType) state->peq[i].type.load(), state->peq[i].freq.load(), state->peq[i].gain.load(), state->peq[i].q.load() };
            if (state->peqSequence.load (std::memory_order_acquire) == seq)    // not rewritten meanwhile
            {
                seenPeq = seq;
                for (size_t i = 0; i < b.size(); ++i) peqC[i] = hpeq::design (b[i], fs);   // a 0 dB band designs to identity
            }
        }
        const bool hp = state->eqActive(), pq = state->peqActive();
        for (int c = 0; c < numChannels; ++c)
        {
            double* x = ch[c];
            for (int i = 0; i < numFrames; ++i)
            {
                double y = x[i] * preamp;
                if (hp) y = runBiquads (eqC.data(), numEq, eqZ[(size_t) c].data(), y);
                if (pq) y = runBiquads (peqC.data(), 7, peqZ[(size_t) c].data(), y);
                x[i] = y;
            }
        }
    }

    static double runBiquads (const hpeq::Coeffs* k, int n, std::array<double, 2>* z, double y) noexcept
    {
        for (int b = 0; b < n; ++b)
        {
            const double out = k[b].b0 * y + z[b][0];          // transposed direct form II
            z[b][0] = k[b].b1 * y - k[b].a1 * out + z[b][1];
            z[b][1] = k[b].b2 * y - k[b].a2 * out;
            y = out;
        }
        return y;
    }

    std::shared_ptr<StudioToolsState> state;
    double fs = 44100.0, preamp = 1.0;
    int seenEqVersion = -1, numEq = 0, seenPeq = -1;
    std::array<hpeq::Coeffs, 8> eqC {};
    std::array<hpeq::Coeffs, 7> peqC {};
    std::array<std::array<std::array<double, 2>, 8>, 2> eqZ {}, peqZ {};
    double a0Lo = 0, b1Lo = 0, a0Hi = 0, a1Hi = 0, b1Hi = 0, gain = 1;
    std::array<double, 2> lo {}, hi {}, asis {};
};

inline juce::String StudioToolsState::describe() const
{
    juce::StringArray parts;
    switch (monitor.load())
    {
        case Monitor::Mono:     parts.add ("Mono (L+R)"); break;
        case Monitor::MidSolo:  parts.add ("Solo Mid"); break;
        case Monitor::SideSolo: parts.add ("Solo Side"); break;
        case Monitor::Stereo:   break;
    }
    if (invertPhase.load()) parts.add (juce::String::fromUTF8 ("Phase \xc3\x98"));
    if (crossfeed.load()) parts.add (monitor.load() == Monitor::Stereo ? "Crossfeed" : "Crossfeed (inactif en mono)");
    if (eqActive()) parts.add ("EQ " + eqPresetInfo()->displayName());
    if (peqActive()) parts.add (juce::String::fromUTF8 ("EQ param\xc3\xa9trique"));
    return parts.isEmpty() ? juce::String ("aucun") : parts.joinIntoString (" + ");
}

} // namespace nx
