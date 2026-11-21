// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "PlaybackPlan.h"

namespace nx
{

namespace
{
    bool supports (const DeviceCapabilities& caps, double rate)
    {
        for (auto r : caps.sampleRates)
            if (std::abs (r - rate) < 0.5) return true;
        return false;
    }

    double bestPcmRate (const DeviceCapabilities& caps, double wanted)
    {
        if (supports (caps, wanted) || caps.sampleRates.isEmpty()) return wanted;

        // 1. smallest supported integer multiple of the source (pure upsampling)
        double best = 0.0;
        for (auto r : caps.sampleRates)
            if (r > wanted && std::fmod (r, wanted) == 0.0 && (best == 0.0 || r < best)) best = r;
        if (best > 0.0) return best;

        // 2. largest supported rate of the same family (pure decimation or ×2^k)
        for (auto r : caps.sampleRates)
            if (isFamily441 (r) == isFamily441 (wanted) && r > best) best = r;
        if (best > 0.0) return best;

        // 3. anything: the highest rate
        for (auto r : caps.sampleRates) best = juce::jmax (best, r);
        return best;
    }
}

PlaybackPlan makePlan (const StreamFormat& src, const DeviceCapabilities& caps, const EngineSettings& s)
{
    PlaybackPlan p;
    p.source = src;
    // Channels. More than the output has (a stereo DAC, a 2-channel profile), or "Stéréo" chosen: a stereo mix,
    // never a refusal. "Multicanal" lifts a profile's 2-channel limit, never the device's own.
    const int deviceOutputs = caps.outputChannels > 0 ? juce::jmin (8, caps.outputChannels) : 8;
    const int outputs = s.channelMode == ChannelMode::Multichannel ? deviceOutputs : juce::jmin (deviceOutputs, juce::jlimit (1, 8, s.maxChannels));
    const bool multi = src.numChannels > 2;
    p.downmix = multi && (s.channelMode == ChannelMode::Stereo || src.numChannels > outputs);
    p.channels = p.downmix || src.numChannels == 1 ? 2 : src.numChannels;
    for (int o = 0; o < 8; ++o) p.route[(size_t) o] = src.numChannels == 1 ? (o < 2 ? 0 : -1) : (o < p.channels ? o : -1);
    if (multi && ! p.downmix)
    {
        // speakers: each source channel to its own existing output, otherwise the plain order
        std::array<bool, 8> used {};
        int need = 0;
        bool ok = true;
        for (int c = 0; c < src.numChannels && ok; ++c)
        {
            const int m = s.channelMap[(size_t) c];
            ok = m >= 0 && m < outputs && ! used[(size_t) m];
            if (ok) { used[(size_t) m] = true; need = juce::jmax (need, m + 1); }
        }
        if (ok)
        {
            p.channels = need;
            p.route.fill (-1);
            for (int c = 0; c < src.numChannels; ++c) p.route[(size_t) s.channelMap[(size_t) c]] = c;
        }
    }
    const juce::String foldNote = p.downmix ? " · "_t + juce::String (src.numChannels) + " canaux -> stéréo (mixage ITU)"_t : juce::String();
    p.filterShape = s.filterShape;
    p.profileTag = s.profileTag;
    const juce::String filterName = s.filterShape == FilterShape::MinimumPhase ? juce::String ("filtre à phase minimale 64 bits"_t)
                                                                               : juce::String ("filtre phase linéaire 64 bits"_t);

    if (src.isDsd())
    {
        // a stereo mix of a multichannel DSD stream can only be computed in PCM (DXD)
        const bool nativeOk = ! s.dspNeedsPcm && ! p.downmix && caps.nativeDsdRates.contains (src.sampleRate);
        const bool dopOk = ! s.dspNeedsPcm && ! p.downmix && s.dopAllowed && supports (caps, src.sampleRate / 16.0);

        if ((s.dsdMode == DsdMode::Auto || s.dsdMode == DsdMode::Native) && nativeOk)
        {
            p.wire = WireFormat::NativeDsd;
            p.deviceRate = src.sampleRate;
            p.ringBitsHint = 1;
            p.reason = "DSD natif (ASIO) : le flux 1 bit part tel quel vers le DAC" + foldNote;
            return p;
        }
        if ((s.dsdMode == DsdMode::Auto || s.dsdMode == DsdMode::DoP || (s.dsdMode == DsdMode::Native && ! nativeOk)) && dopOk)
        {
            p.wire = WireFormat::DoP;
            p.deviceRate = src.sampleRate / 16.0;
            p.ringBitsHint = 24;
            p.reason = "DoP : le flux DSD voyage intact dans des mots PCM de 24 bits à "_t + formatRate (p.deviceRate) + foldNote;
            return p;
        }

        // DSD -> PCM: the highest 44,1 kHz-family rate the DAC accepts, DXD at most.
        double target = 0.0;
        for (double r : { 352800.0, 176400.0, 88200.0, 44100.0 })
            if (r <= s.maxDsdToPcmRate && r <= src.sampleRate / 8.0 && supports (caps, r)) { target = r; break; }

        p.wire = WireFormat::Pcm;
        p.conversion = PlaybackPlan::Conversion::DsdToPcm;
        if (target > 0.0)
        {
            p.conversionRate = target;
            p.deviceRate = target;
        }
        else
        {
            p.conversionRate = juce::jmin (352800.0, src.sampleRate / 8.0);
            p.deviceRate = bestPcmRate (caps, p.conversionRate);
        }
        p.ringBitsHint = 32;
        p.needsRequantize = true;
        p.reason = juce::String (p.downmix ? "Mixage stéréo ITU de "_t + juce::String (src.numChannels) + " canaux : "_t
                                 : s.dspNeedsPcm ? "Outils Studio actifs : "_t
                                 : s.profileTag.isNotEmpty() ? "Profil "_t + s.profileTag + " : "
                                 : caps.nativeDsdRates.isEmpty() && ! dopOk ? "Le DAC n'accepte ni DSD natif ni DoP à ce débit : "_t
                                                                            : "Conversion demandée : "_t)
                   + "DSD -> PCM " + formatRate (p.deviceRate) + " (" + filterName + ")" + (p.downmix ? juce::String() : foldNote);
        return p;
    }

    // PCM
    double wanted = src.sampleRate;
    if (s.upsampleToDxd)
    {
        const double dxd = isFamily441 (src.sampleRate) ? 352800.0 : 384000.0;
        for (double r = dxd; r > src.sampleRate; r *= 0.5)
            if (supports (caps, r) && std::fmod (r, src.sampleRate) == 0.0) { wanted = r; break; }
    }
    p.deviceRate = bestPcmRate (caps, wanted);

    if (p.deviceRate != src.sampleRate)
    {
        p.conversion = PlaybackPlan::Conversion::Resample;
        p.ringBitsHint = 32;
        p.needsRequantize = true;
        p.reason = wanted != src.sampleRate && p.deviceRate == wanted
                       ? "Suréchantillonnage sinc "_t + formatRate (src.sampleRate) + " -> " + formatRate (p.deviceRate) + " (" + filterName + ")"
                       : "Le DAC n'accepte pas " + formatRate (src.sampleRate) + " : conversion vers " + formatRate (p.deviceRate);
    }
    else if (src.kind == SampleKind::PcmFloat)
    {
        p.conversion = PlaybackPlan::Conversion::FloatToInt;
        p.ringBitsHint = 32;
        p.needsRequantize = true;
        p.reason = src.lossy ? "Source compressée avec pertes ("_t + src.codec + ")"
                             : "Source en virgule flottante : quantification vers la résolution du DAC"_t;
    }
    else
    {
        p.ringBitsHint = juce::jlimit (16, 32, src.bitsPerSample);
        p.needsRequantize = src.bitsPerSample > caps.maxBits;
        p.reason = p.needsRequantize ? "Le DAC est limité à "_t + juce::String (caps.maxBits) + " bits"
                                     : "Bit-perfect : aucun traitement";
    }
    if (p.downmix)
    {
        // the 5.1 -> 2.0 mix is computed in 64-bit float, then dithered to the DAC's word
        p.ringBitsHint = 32;
        p.needsRequantize = true;
        p.reason += foldNote;
    }
    return p;
}

} // namespace nx
