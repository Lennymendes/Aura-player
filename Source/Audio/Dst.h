// NX Player — Copyright (c) 2026 Lenny Antunes Mendes (NX Audio). Proprietary, all rights reserved (see LICENSE).
// DST (Direct Stream Transfer) decoder: the lossless 1-bit coding of SACD discs and DSDIFF "DST " files,
// ISO/IEC 14496-3 subpart 10. Written from the standard's decoding process (the MPEG-4 reference decoder
// and FFmpeg's dstdec were used to check behaviour): segmentation and mapping of filters / probability
// tables, Rice-coded filter coefficients and Ptables, binary FIR prediction (up to 128 taps) and the 12-bit
// arithmetic decoder. Each frame is independent (1/75 s); the output is DSD, MSB first, interleaved by byte
// (the DFF / SACD layout).
#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <vector>

namespace nx::dst
{

constexpr int maxChannels = 6;

class FrameDecoder
{
public:
    /** fsMultiple: 64 for DSD64 (4704 bytes per channel and per frame), 128, 256. */
    explicit FrameDecoder (int numChannels, int fsMultiple = 64);

    int getNumChannels() const noexcept     { return channels; }
    int getBytesPerChannel() const noexcept { return frameBytes; }
    int getOutputSize() const noexcept      { return frameBytes * channels; }

    /** Decodes one frame into getOutputSize() bytes. On a corrupt frame, writes DSD silence and returns false. */
    bool decode (const uint8_t* frame, size_t size, uint8_t* out, juce::String& error);

    /** True if the last frame decoded was stored uncompressed (DST_Coded = 0). */
    bool lastFrameWasPlain() const noexcept { return plain; }

private:
    struct Bits;
    struct Segments
    {
        int resolution = 1;
        std::array<int, maxChannels> count {};
        std::array<std::array<int, 9>, maxChannels> length {};   // up to 8 coded segments + the implicit last one
        std::array<std::array<int, 9>, maxChannels> table {};
    };

    bool readSegments (Bits&, int maxSegments, int minSegmentBits, Segments&, juce::String& error) const;
    bool readMapping (Bits&, int maxTables, Segments&, int& numTables, juce::String& error) const;
    void fillTable4Bit (const Segments&, std::vector<uint8_t>& out) const;

    int channels, frameBytes, bitsPerChannel;
    bool plain = false;

    int numFilters = 0, numPtables = 0;
    std::array<int, 2 * maxChannels> filterOrder {}, ptableLength {};
    std::array<std::array<int, 128>, 2 * maxChannels> coef {};
    std::array<std::array<int, 64>, 2 * maxChannels> ptable {};
    std::array<bool, maxChannels> halfProb {};
    std::vector<int16_t> filterTables;          // [filter][16][256]: sum of 8 taps for each byte of history
    std::vector<uint8_t> filter4Bit, ptable4Bit; // [channel][bit]
};

} // namespace nx::dst
