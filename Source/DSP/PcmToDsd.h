// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// PCM -> DSD (DSD64 / 128 / 256): the remodulator, in 64-bit floating point.
//   1. headroom gain: PCM 0 dBFS -> SACD 0 dB (50 % modulation) x headroom (-3 dB by default);
//   2. look-ahead peak limiter at the DXD rate (inter-sample peaks are visible there: the signal is 8x oversampled),
//      ceiling 0.45 (-0.9 dB under SACD 0 dB), so the modulator never sees a level it cannot code;
//   3. polyphase FIR interpolation DXD -> DSD rate (x8 / x16 / x32, Kaiser, 16 taps per phase);
//   4. 1-bit sigma-delta modulator, 5th or 7th order, error-feedback form with the noise transfer function as a
//      cascade of 2nd-order sections: zeros spread over 0-20 kHz (Legendre roots, the least in-band noise),
//      Butterworth high-pass poles with |NTF| <= 1.5 out of band (Lee's rule: stable with a 1-bit quantiser).
//      A guard resets the loop if it ever diverges (counted; never seen under the ceiling).
// Everything is allocated in prepare(); process() is real-time safe.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nx::dsp
{

class PcmToDsd
{
public:
    static constexpr double ceiling = 0.45;      // modulator input peak (1-bit full scale = 1; SACD 0 dB = 0.5)

    /** inRate: the DXD rate of the input (352 800 or 384 000 Hz); dsdRate = inRate x 8 / 16 / 32. order: 5 or 7. */
    void prepare (double inRate, double dsdRate, int numChannels, int order, double headroomDb);
    void reset() noexcept;

    int getRatio() const noexcept { return ratio; }                    // DSD bits per input frame and channel
    int getBytesPerInputFrame() const noexcept { return ratio / 8; }
    /** Delay through the limiter and the interpolator, in input frames (silence to feed at the end). */
    int getLatencyInputFrames() const noexcept { return lookahead + taps; }

    /** frames interleaved doubles in, frames x getBytesPerInputFrame() bytes per channel out (interleaved by byte,
        MSB = earliest bit, 1 = positive). Returns the number of bytes per channel written. */
    int process (const double* in, int frames, uint8_t* out) noexcept;

    /** Noise transfer function magnitude at a normalised frequency (rad / sample at the DSD rate). */
    double ntfMagnitude (double w) const noexcept;

    std::uint64_t limitedFrames = 0;             // frames where the limiter pulled the gain down
    std::uint64_t loopResets = 0;                // modulator states reset after a divergence

private:
    struct Section { double b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    int channels = 2, ratio = 8, order = 7, taps = 16, lookahead = 256;
    double gain = 0.5;
    std::vector<Section> sections;
    std::vector<double> coeffs;                  // [phase][tap]
    // per channel
    std::vector<double> history;                 // interpolator input, newest first (2 x taps, mirrored)
    std::vector<int> historyPos;
    std::vector<double> state;                   // [channel][section][2]
    // limiter (shared gain for all channels: the image stays put)
    std::vector<double> delayLine;               // [lookahead][channel]
    std::vector<double> holdValue;
    std::vector<int> holdIndex;
    std::vector<double> boxRing;
    int delayPos = 0, holdHead = 0, holdTail = 0, boxPos = 0;
    std::int64_t frameCount = 0;
    double boxSum = 0;
    std::vector<double> upsampled;               // one input frame, all channels: [channel][ratio]
};

} // namespace nx::dsp
