// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// How a track reaches the DAC: decided once per track from its format, the DAC's capabilities and the
// user's settings. Two consecutive tracks with the same "wire" (format, rate, channels) are played
// gaplessly; a different wire reopens the device at the track boundary.
#pragma once

#include <array>

#include "../Core/Format.h"
#include "../Output/OutputBackend.h"

namespace nx
{

enum class DsdMode { Auto, Native, DoP, ConvertToPcm };

/** Hardware profile: which device-specific optimisation applies. Auto = recognised from the device name. */
enum class HardwareProfile { Auto, Generic, MergingRavenna, RmeAdi2, FiioKa17 };

/** Sound profiles of a Merging Anubis / RAVENNA (AES67) interface, chosen in one click. */
enum class AnubisProfile
{
    StudioDxdReference,   // DSD -> DXD 352,8 kHz (linear phase, gain + limiter), PCM bit-perfect: no DoP on the network
    AnalogWarmth,         // DSD and PCM -> DXD with a minimum-phase filter + optional even-harmonic / HF shaping
    PureDoP               // DSD in DoP, "RAVENNA Sync Safe": large buffer, 2 channels, 1,5 s of DoP silence at each rate change
};

/** Sound profiles of an RME ADI-2 (USB). */
enum class RmeProfile
{
    HighPrecisionDoP,       // DSD in DoP 1.0 (up to DSD256), 150 ms sync, PCM bit-perfect
    VelvetDxd             // DSD -> DXD 352,8 kHz +6 dB, minimum phase: keeps the ADI-2's DSP (EQ, loudness, crossfeed) usable
};

enum class FilterShape { LinearPhase, MinimumPhase };
/** Multichannel sources (5.1 SACD, DSF / DFF, WAV): Auto = what the output can play (and the device profile allows);
    Stereo = always two channels (the disc's stereo area, otherwise an ITU mix computed in DXD); Multichannel = every
    channel whenever the output has them, even under a 2-channel device profile. */
enum class ChannelMode { Auto, Stereo, Multichannel };

struct EngineSettings
{
    DsdMode dsdMode = DsdMode::Auto;
    bool dopAllowed = true;              // DoP needs a DAC that recognises the markers: on by default
    bool upsampleToDxd = false;          // "sinc vers DXD": integer-ratio upsampling to 352,8 / 384 kHz
    double dsdToPcmGainDb = 0.0;         // 0 dB: SACD 0 dB -> −6 dBFS, never clips; +6 dB: louder
    double maxDsdToPcmRate = 352800.0;
    bool exclusive = true;               // hog mode / WASAPI exclusive
    int bufferMilliseconds = 0;          // 0 = backend default
    bool freeEdition = false;            // Aura Free: no DSD / DXD / SACD, no exclusive mode, no device profiles
    bool memoryPlay = true;              // Memory Play: each local file is copied into RAM, the disk is idle while it plays
    int memoryPlayMaxMb = 1536;          // larger files are read from the disk
    bool dspNeedsPcm = false;            // studio tools on: DSD is converted to PCM so that they can apply (not saved)
    ChannelMode channelMode = ChannelMode::Auto;
    /** PCM -> DSD: 0 = off, 64 / 128 / 256 = remodulated to that DSD rate (DoP or native) when the DAC takes it. */
    int pcmToDsd = 0;
    int sdmOrder = 7;                    // sigma-delta order: 5 or 7
    double pcmToDsdHeadroomDb = -3.0;    // PCM 0 dBFS -> SACD 0 dB + this (the limiter caps at -0.9 dB SACD)
    /** Multichannel output: source channel c (SACD / WAV order: L R C LFE Ls Rs...) goes to device output channelMap[c]. */
    std::array<int, 8> channelMap { 0, 1, 2, 3, 4, 5, 6, 7 };

    // Device profiles (see DeviceProfiles.h): resolved against the device name at each track.
    HardwareProfile hardwareProfile = HardwareProfile::Auto;
    AnubisProfile anubisProfile = AnubisProfile::StudioDxdReference;
    double anubisDsdGainDb = 6.0;        // DSD -> DXD gain in the Anubis profiles: 0, +3 or +6 dB (limiter above 0 dBFS)
    bool anubisHarmonics = true;         // "Analog Warmth": the harmonic / HF shaping stage (can be switched off)
    RmeProfile rmeProfile = RmeProfile::HighPrecisionDoP;
    int fiioProfile = 0;                 // FiiO KA17: 0 = DoP bit-perfect, 1 = DSD -> DXD
    juce::String deviceModel;            // equipment chosen by hand in the catalogue ("" = none); see DeviceModels
    bool rmeLockVolume = true;           // "Sécurité RME DSD Direct": software volume locked at 100 %

    // Filled by resolveProfile() — the effective processing of the current device.
    FilterShape filterShape = FilterShape::LinearPhase;
    bool limiter = false;                // soft peak limiter after a gain > 0 dB
    bool warmth = false;                 // even-harmonic (−90 dB) + gentle shelf above 18 kHz
    int bufferFrames = 0;                // forced device buffer, in frames (0 = use bufferMilliseconds)
    int maxChannels = 8;                 // channels opened on the device
    int dopPrerollMs = 0;                // DoP silence (0x69 + markers) before the music after a rate change
    juce::String profileTag;             // shown in the badge: "Anubis Ref", "Warm", "Bit-Perfect"
    bool lockVolume = false;             // software volume held at 100 % (bit-perfect, RME DSD Direct)
};

struct PlaybackPlan
{
    StreamFormat source;
    WireFormat wire = WireFormat::Pcm;
    double deviceRate = 44100.0;         // PCM / DoP word rate, DSD bit rate in native DSD
    int channels = 2;                    // channels sent to the device (mono is doubled)
    bool downmix = false;                // multichannel source on a stereo output: ITU 5.1 -> 2.0 mix in 64 bits (DSD through DXD)
    bool stereoLayer = false;            // SACD image: the disc's own stereo area plays instead of the multichannel one
    /** Device output o plays source channel route[o] (-1: silence). Identity for a plain stereo / multichannel file. */
    std::array<int, 8> route { 0, 1, 2, 3, 4, 5, 6, 7 };
    bool identityRoute() const noexcept
    {
        if (channels != source.numChannels) return false;
        for (int o = 0; o < channels; ++o) if (route[(size_t) o] != o) return false;
        return true;
    }
    int ringBitsHint = 24;               // significant bits stored in the ring (requested from the device)

    enum class Conversion { None, FloatToInt, Resample, DsdToPcm, PcmToDsd } conversion = Conversion::None;
    double conversionRate = 0.0;         // intermediate rate (DSD -> PCM output before an optional resample)
    bool needsRequantize = false;        // the ring holds more precision than the device word: TPDF dither

    juce::String reason;                 // why (shown in the Signal Path)
    FilterShape filterShape = FilterShape::LinearPhase;
    juce::String profileTag;             // active device profile ("Anubis Ref"...), empty if none
    bool inMemory = false;               // Memory Play: the file is (being) copied into RAM
    juce::int64 fileBytes = 0;

    /** Same device configuration: the boundary between two tracks can be gapless. */
    bool sameWire (const PlaybackPlan& o) const noexcept
    {
        return wire == o.wire && deviceRate == o.deviceRate && channels == o.channels
            && (wire != WireFormat::Pcm || ringBitsHint == o.ringBitsHint);
    }

    int ringFrameBytes() const noexcept { return wire == WireFormat::Pcm ? channels * 4 : channels; }

    /** Frames of device output per ring frame unit (DoP: one PCM frame per 2 DSD bytes). */
    double ringUnitsPerSecond() const noexcept
    {
        switch (wire)
        {
            case WireFormat::Pcm:       return deviceRate;
            case WireFormat::DoP:       return deviceRate * 2.0;  // bytes per channel per second
            case WireFormat::NativeDsd: return deviceRate / 8.0;
        }
        return deviceRate;
    }
};

PlaybackPlan makePlan (const StreamFormat& source, const DeviceCapabilities& caps, const EngineSettings& settings);

} // namespace nx
