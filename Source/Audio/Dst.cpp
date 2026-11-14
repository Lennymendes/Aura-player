// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Dst.h"
#include "../Core/Text.h"
#include <cstring>

namespace nx::dst
{

namespace
{
    constexpr int maxFilterSegments = 4, maxPtableSegments = 8;
    constexpr int minFilterSegmentBits = 1024, minPtableSegmentBits = 32;
    constexpr uint8_t dsdSilence = 0x69;

    // prediction of the coded filter coefficients / Ptable entries (method 0, 1, 2 = order 1, 2, 3)
    constexpr int filterPred[3][3] = { { -8, 0, 0 }, { -16, 8, 0 }, { -9, -5, 6 } };
    constexpr int ptablePred[3][3] = { { -8, 0, 0 }, { -16, 8, 0 }, { -24, 24, -8 } };

    /** Number of bits to code a value in [0, x]: floor (log2 x) + 1 (1 for x = 1, 0 for x = 0). */
    int bitsFor (int x) { int y = 0; while (x >= (1 << y)) ++y; return y; }

    /** The probability of the first ("DST_X_Bit") decision: the 7 LSBs of the first coefficient, reversed, plus 1. */
    int reverse7Plus1 (int c)
    {
        int r = 0;
        for (int i = 0; i < 7; ++i) r |= ((c >> i) & 1) << (6 - i);
        return r + 1;
    }
}

struct FrameDecoder::Bits
{
    const uint8_t* data;
    size_t size, pos = 0;
    bool overrun = false;
    int bit() noexcept
    {
        if (pos >= size) { overrun = true; return 0; }
        const int b = (data[pos >> 3] >> (7 - (pos & 7))) & 1;
        ++pos;
        return b;
    }
    int get (int n) noexcept { int v = 0; while (n-- > 0) v = (v << 1) | bit(); return v; }
    int getSigned (int n) noexcept { const int v = get (n); return (v & (1 << (n - 1))) != 0 ? v - (1 << n) : v; }
    int rice (int m) noexcept
    {
        int run = 0;
        while (bit() == 0) { if (overrun || ++run > 1 << 16) return 0; }
        const int n = (run << m) + get (m);
        return n != 0 && bit() != 0 ? -n : n;
    }
};

FrameDecoder::FrameDecoder (int numChannels, int fsMultiple)
    : channels (juce::jlimit (1, maxChannels, numChannels)),
      frameBytes (588 * juce::jlimit (64, 256, fsMultiple) / 8),
      bitsPerChannel (frameBytes * 8)
{
    filterTables.resize ((size_t) 2 * maxChannels * 16 * 256);
    filter4Bit.resize ((size_t) channels * (size_t) bitsPerChannel);
    ptable4Bit.resize ((size_t) channels * (size_t) bitsPerChannel);
}

bool FrameDecoder::readSegments (Bits& b, int maxSegments, int minSegmentBits, Segments& s, juce::String& error) const
{
    const int maxResolution = frameBytes - minSegmentBits / 8;
    int definedBits = 0, maxSegSize = maxResolution, seg = 0;
    bool resolutionRead = false;

    auto readOne = [&] (int ch) -> bool
    {
        if (seg >= maxSegments) { error = "DST : trop de segments"_t; return false; }
        if (! resolutionRead)
        {
            s.resolution = b.get (bitsFor (maxResolution));
            if (s.resolution == 0 || s.resolution > maxResolution) { error = "DST : résolution de segment invalide"_t; return false; }
            resolutionRead = true;
        }
        const int len = b.get (bitsFor (maxSegSize / s.resolution));
        const int lenBits = s.resolution * 8 * len;
        if (lenBits < minSegmentBits || lenBits > bitsPerChannel - definedBits - minSegmentBits) { error = "DST : longueur de segment invalide"_t; return false; }
        s.length[(size_t) ch][(size_t) seg] = len;
        definedBits += lenBits;
        maxSegSize -= s.resolution * len;
        ++seg;
        return true;
    };

    const bool sameForAll = b.bit() != 0;
    if (sameForAll)
    {
        while (b.bit() == 0)                                   // end of channel?
            if (b.overrun || ! readOne (0)) { if (error.isEmpty()) error = "DST : trame tronquée"_t; return false; }
        s.count[0] = seg + 1;
        s.length[0][(size_t) seg] = 0;
        for (int ch = 1; ch < channels; ++ch) { s.count[(size_t) ch] = s.count[0]; s.length[(size_t) ch] = s.length[0]; }
    }
    else
    {
        for (int ch = 0; ch < channels;)
        {
            if (seg >= maxSegments) { error = "DST : trop de segments"_t; return false; }   // checked before the end-of-channel bit
            if (b.bit() == 0)
            {
                if (b.overrun || ! readOne (ch)) { if (error.isEmpty()) error = "DST : trame tronquée"_t; return false; }
            }
            else
            {
                s.count[(size_t) ch] = seg + 1;
                s.length[(size_t) ch][(size_t) seg] = 0;
                seg = 0; definedBits = 0; maxSegSize = maxResolution;
                ++ch;
            }
        }
    }
    if (! resolutionRead) s.resolution = 1;
    return ! b.overrun;
}

bool FrameDecoder::readMapping (Bits& b, int maxTables, Segments& s, int& numTables, juce::String& error) const
{
    int count = 1;
    auto readTable = [&] (int ch, int seg) -> bool
    {
        const int t = b.get (bitsFor (count));
        if (t == count) ++count;
        else if (t > count) { error = "DST : numéro de table invalide"_t; return false; }
        s.table[(size_t) ch][(size_t) seg] = t;
        return true;
    };
    s.table[0][0] = 0;
    if (b.bit() != 0)                                          // same mapping for all channels
    {
        for (int seg = 1; seg < s.count[0]; ++seg) if (! readTable (0, seg)) return false;
        for (int ch = 1; ch < channels; ++ch)
        {
            if (s.count[(size_t) ch] != s.count[0]) { error = "DST : correspondance commune impossible"_t; return false; }
            s.table[(size_t) ch] = s.table[0];
        }
    }
    else
    {
        for (int ch = 0; ch < channels; ++ch)
            for (int seg = 0; seg < s.count[(size_t) ch]; ++seg)
                if ((ch != 0 || seg != 0) && ! readTable (ch, seg)) return false;
    }
    if (count > maxTables) { error = "DST : trop de tables"_t; return false; }
    numTables = count;
    return ! b.overrun;
}

void FrameDecoder::fillTable4Bit (const Segments& s, std::vector<uint8_t>& out) const
{
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* row = out.data() + (size_t) ch * (size_t) bitsPerChannel;
        int start = 0, seg = 0;
        for (; seg < s.count[(size_t) ch] - 1; ++seg)
        {
            const int end = juce::jmin (bitsPerChannel, start + s.resolution * 8 * s.length[(size_t) ch][(size_t) seg]);
            std::memset (row + start, s.table[(size_t) ch][(size_t) seg], (size_t) (end - start));
            start = end;
        }
        std::memset (row + start, s.table[(size_t) ch][(size_t) seg], (size_t) (bitsPerChannel - start));
    }
}

bool FrameDecoder::decode (const uint8_t* frame, size_t size, uint8_t* out, juce::String& error)
{
    const size_t outSize = (size_t) getOutputSize();
    auto fail = [&] (const juce::String& why)
    {
        error = why;
        std::memset (out, dsdSilence, outSize);
        return false;
    };
    if (frame == nullptr || size == 0) return fail ("DST : trame vide"_t);

    Bits b { frame, size * 8 };
    if (b.bit() == 0)
    {
        // DST_Coded = 0: one header byte (DST_X_Bit, 6 stuffing zeros), then the plain DSD of every channel
        plain = true;
        b.bit();
        if (b.get (6) != 0) return fail ("DST : motif de bourrage invalide"_t);
        const size_t n = juce::jmin (outSize, size - 1);
        std::memcpy (out, frame + 1, n);
        std::memset (out + n, dsdSilence, outSize - n);
        return true;
    }
    plain = false;

    // segmentation (filters, then Ptables or "same as filters")
    Segments fseg, pseg;
    const bool pSameSegAsF = b.bit() != 0;
    if (! readSegments (b, maxFilterSegments, minFilterSegmentBits, fseg, error)) return fail (error);
    if (pSameSegAsF)
    {
        pseg = fseg;
        for (int ch = 0; ch < channels; ++ch)
        {
            if (pseg.count[(size_t) ch] > maxPtableSegments) return fail ("DST : trop de segments"_t);
            for (int seg = 0; seg < pseg.count[(size_t) ch]; ++seg)
            {
                const int len = pseg.length[(size_t) ch][(size_t) seg];
                if (len != 0 && pseg.resolution * 8 * len < minPtableSegmentBits) return fail ("DST : longueur de segment invalide"_t);
            }
        }
    }
    else if (! readSegments (b, maxPtableSegments, minPtableSegmentBits, pseg, error)) return fail (error);

    // mapping of the segments to filters / Ptables
    const bool pSameMapAsF = b.bit() != 0;
    if (! readMapping (b, 2 * channels, fseg, numFilters, error)) return fail (error);
    if (pSameMapAsF)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            if (pseg.count[(size_t) ch] != fseg.count[(size_t) ch]) return fail ("DST : segments des filtres et des tables différents"_t);
            pseg.table[(size_t) ch] = fseg.table[(size_t) ch];
        }
        numPtables = numFilters;
    }
    else if (! readMapping (b, 2 * channels, pseg, numPtables, error)) return fail (error);

    for (int ch = 0; ch < channels; ++ch) halfProb[(size_t) ch] = b.bit() != 0;

    // filter coefficients
    for (int f = 0; f < numFilters; ++f)
    {
        auto& c = coef[(size_t) f];
        const int order = b.get (7) + 1;
        filterOrder[(size_t) f] = order;
        int i = 0;
        if (b.bit() == 0)
        {
            for (; i < order; ++i) c[(size_t) i] = b.getSigned (9);
        }
        else
        {
            const int method = b.get (2);
            if (method > 2 || method + 1 >= order) return fail ("DST : codage des coefficients invalide"_t);
            for (; i <= method; ++i) c[(size_t) i] = b.getSigned (9);
            const int m = b.get (3);
            for (; i < order; ++i)
            {
                int x = 0;
                for (int k = 0; k <= method; ++k) x += filterPred[method][k] * c[(size_t) (i - k - 1)];
                const int r = b.rice (m);
                const int v = x >= 0 ? r - (x + 4) / 8 : r + (-x + 3) / 8;
                if (v < -256 || v > 255) return fail ("DST : coefficient de filtre hors limites"_t);
                c[(size_t) i] = v;
            }
        }
        for (; i < 128; ++i) c[(size_t) i] = 0;
        if (b.overrun) return fail ("DST : trame tronquée"_t);
    }

    // probability tables
    for (int t = 0; t < numPtables; ++t)
    {
        auto& p = ptable[(size_t) t];
        const int len = b.get (6) + 1;
        ptableLength[(size_t) t] = len;
        if (len == 1) { p[0] = 128; continue; }
        if (b.bit() == 0)
        {
            for (int i = 0; i < len; ++i) p[(size_t) i] = b.get (7) + 1;
        }
        else
        {
            const int method = b.get (2);
            if (method > 2 || method + 1 >= len) return fail ("DST : codage des tables de probabilité invalide"_t);
            int i = 0;
            for (; i <= method; ++i) p[(size_t) i] = b.get (7) + 1;
            const int m = b.get (3);
            for (; i < len; ++i)
            {
                int x = 0;
                for (int k = 0; k <= method; ++k) x += ptablePred[method][k] * p[(size_t) (i - k - 1)];
                const int r = b.rice (m);
                const int v = x >= 0 ? r - (x + 4) / 8 : r + (-x + 3) / 8;
                if (v < 1 || v > 128) return fail ("DST : table de probabilité hors limites"_t);
                p[(size_t) i] = v;
            }
        }
        if (b.overrun) return fail ("DST : trame tronquée"_t);
    }

    // arithmetic coded data: the rest of the frame, its first bit is 0
    const size_t aStart = b.pos;
    const size_t aLen = size * 8 - aStart;
    auto abit = [&] (size_t k) -> unsigned { return k < aLen ? (unsigned) ((frame[(aStart + k) >> 3] >> (7 - ((aStart + k) & 7))) & 1) : 0u; };
    if (aLen > 0 && abit (0) != 0) return fail ("DST : code arithmétique invalide"_t);

    // look-up tables: for each filter, 16 tables of the sum of 8 taps for every byte of history
    for (int f = 0; f < numFilters; ++f)
        for (int t = 0; t < 16; ++t)
        {
            auto* tab = filterTables.data() + ((size_t) f * 16 + (size_t) t) * 256;
            const int taps = juce::jlimit (0, 8, filterOrder[(size_t) f] - t * 8);
            for (int v = 0; v < 256; ++v)
            {
                int sum = 0;
                for (int j = 0; j < taps; ++j) sum += (((v >> j) & 1) * 2 - 1) * coef[(size_t) f][(size_t) (t * 8 + j)];
                tab[v] = (int16_t) sum;
            }
        }
    fillTable4Bit (fseg, filter4Bit);
    fillTable4Bit (pseg, ptable4Bit);

    std::array<int, maxChannels> halfBits {};
    for (int ch = 0; ch < channels; ++ch) halfBits[(size_t) ch] = filterOrder[(size_t) fseg.table[(size_t) ch][0]];

    // arithmetic decoder (12-bit registers; zeros are read past the end of the code)
    unsigned A = 4095, C = 0;
    size_t cbptr = 1;
    for (; cbptr <= 12; ++cbptr) C = (C << 1) | abit (cbptr);
    auto decodeBit = [&] (unsigned p) -> unsigned
    {
        const unsigned ap = ((A >> 8) | ((A >> 7) & 1)) * p;
        const unsigned h = A - ap;
        unsigned bitOut;
        if (C >= h) { bitOut = 0; C -= h; A = ap; }
        else        { bitOut = 1; A = h; }
        while (A < 2048) { A <<= 1; C = (C << 1) | abit (cbptr); ++cbptr; }
        return bitOut;
    };

    decodeBit ((unsigned) reverse7Plus1 (coef[0][0]));        // DST_X_Bit: decoded, not used

    std::memset (out, 0, outSize);
    std::array<uint64_t, maxChannels> lo {}, hi {};            // last 128 bits of each channel, newest in bit 0 of lo
    lo.fill (0xAAAAAAAAAAAAAAAAull);
    hi.fill (0xAAAAAAAAAAAAAAAAull);

    for (int i = 0; i < bitsPerChannel; ++i)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto idx = (size_t) ch * (size_t) bitsPerChannel + (size_t) i;
            const int16_t* tab = filterTables.data() + (size_t) filter4Bit[idx] * 16 * 256;
            const uint64_t l = lo[(size_t) ch], h = hi[(size_t) ch];
            int sum = 0;
            for (int t = 0; t < 8; ++t) sum += tab[t * 256 + (int) ((l >> (8 * t)) & 0xFF)];
            for (int t = 0; t < 8; ++t) sum += tab[(t + 8) * 256 + (int) ((h >> (8 * t)) & 0xFF)];
            const auto predict = (int16_t) sum;                 // 16-bit arithmetic, as specified

            unsigned p;
            if (halfProb[(size_t) ch] && i < halfBits[(size_t) ch]) p = 128;
            else
            {
                const int t = ptable4Bit[idx];
                const int index = juce::jmin ((predict < 0 ? -(int) predict : (int) predict) >> 3, ptableLength[(size_t) t] - 1);
                p = (unsigned) ptable[(size_t) t][(size_t) index];
            }
            const unsigned residual = decodeBit (p);
            const unsigned v = ((((uint16_t) predict) >> 15) ^ residual) & 1;
            out[(size_t) (i >> 3) * (size_t) channels + (size_t) ch] |= (uint8_t) (v << (7 - (i & 7)));
            hi[(size_t) ch] = (h << 1) | (l >> 63);
            lo[(size_t) ch] = (l << 1) | v;
        }
    }
    if (cbptr + 7 < aLen) return fail ("DST : erreur de décodage arithmétique"_t);   // the code must be used up
    return true;
}

} // namespace nx::dst
