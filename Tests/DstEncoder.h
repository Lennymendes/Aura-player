// A DST encoder (ISO/IEC 14496-3 subpart 10) for the test bench: it produces real, standard frames from DSD
// so that the decoder can be checked bit for bit (frames from it were also checked against the MPEG-4
// reference decoder and FFmpeg). Prediction filters are fitted per table (autocorrelation + Levinson),
// Ptables are measured on the frame, coefficients and Ptables are Rice coded when that is shorter, and the
// arithmetic coder is the exact inverse of the decoder's. Options exercise segmentation, per-channel
// mapping and "half probability". Standard library only.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsttest
{

constexpr int frameBytesPerChannel = 4704;
constexpr int bitsPerChannel = frameBytesPerChannel * 8;

struct Options
{
    int filterOrder = 40;            // 1..128
    int ptableLength = 40;           // 2..64
    bool twoSegments = false;        // filters / Ptables split in two segments (same for all channels)
    bool perChannelTables = true;    // one filter / Ptable per channel (and segment); else shared
    bool halfProbOddChannels = true; // channels 1, 3, 5: the first bits use probability 1/2
    bool allowRice = true;
};

struct BitWriter
{
    std::vector<uint8_t> bits;
    void put (int n, unsigned v) { while (n-- > 0) bits.push_back ((uint8_t) ((v >> n) & 1)); }
    void putSigned (int n, int v) { put (n, (unsigned) v & ((1u << n) - 1)); }
    void rice (int m, int value)
    {
        const unsigned a = (unsigned) std::abs (value);
        for (unsigned k = 0; k < (a >> m); ++k) bits.push_back (0);
        bits.push_back (1);
        put (m, a & ((1u << m) - 1));
        if (a != 0) bits.push_back (value < 0 ? 1 : 0);
    }
    static int riceCost (int m, int value) { const unsigned a = (unsigned) std::abs (value); return (int) (a >> m) + 1 + m + (a != 0 ? 1 : 0); }
    std::vector<uint8_t> bytes() const
    {
        std::vector<uint8_t> out ((bits.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i) out[i >> 3] |= (uint8_t) (bits[i] << (7 - (i & 7)));
        return out;
    }
};

inline int bitsFor (int x) { int y = 0; while (x >= (1 << y)) ++y; return y; }
inline int reverse7Plus1 (int c) { int r = 0; for (int i = 0; i < 7; ++i) r |= ((c >> i) & 1) << (6 - i); return r + 1; }

/** Writes a list of coefficients / entries, Rice-coded with the cheapest prediction method when allowed. */
inline void writeTable (BitWriter& w, const std::vector<int>& v, int valueBits, bool isSigned, int offset,
                        const int pred[3][3], int maxM, bool allowRice)
{
    auto raw = [&] (int x) { if (isSigned) w.putSigned (valueBits, x); else w.put (valueBits, (unsigned) (x - offset)); };
    const int n = (int) v.size();
    int bestCost = valueBits * n, bestMethod = -1, bestM = 0;
    auto residual = [&] (int method, int i)
    {
        int x = 0;
        for (int k = 0; k <= method; ++k) x += pred[method][k] * v[(size_t) (i - k - 1)];
        return x >= 0 ? v[(size_t) i] + (x + 4) / 8 : v[(size_t) i] - (-x + 3) / 8;
    };
    if (allowRice)
        for (int method = 0; method < 3; ++method)
        {
            if (method + 1 >= n) break;
            for (int m = 0; m <= maxM; ++m)
            {
                int cost = 2 + 3 + valueBits * (method + 1);
                for (int i = method + 1; i < n; ++i) cost += BitWriter::riceCost (m, residual (method, i));
                if (cost < bestCost) { bestCost = cost; bestMethod = method; bestM = m; }
            }
        }
    if (bestMethod < 0)
    {
        w.put (1, 0);
        for (int x : v) raw (x);
        return;
    }
    w.put (1, 1);
    w.put (2, (unsigned) bestMethod);
    for (int i = 0; i <= bestMethod; ++i) raw (v[(size_t) i]);
    w.put (3, (unsigned) bestM);
    for (int i = bestMethod + 1; i < n; ++i) w.rice (bestM, residual (bestMethod, i));
}

/** An uncompressed frame (DST_Coded = 0): header byte + the DSD as it is. */
inline std::vector<uint8_t> plainFrame (const uint8_t* dsd, int channels)
{
    std::vector<uint8_t> f (1 + (size_t) frameBytesPerChannel * (size_t) channels, 0);
    std::memcpy (f.data() + 1, dsd, f.size() - 1);
    return f;
}

/** dsd: channels × 4704 bytes, MSB first, interleaved by byte. */
inline std::vector<uint8_t> encodeFrame (const uint8_t* dsd, int channels, const Options& o = {})
{
    static const int filterPred[3][3] = { { -8, 0, 0 }, { -16, 8, 0 }, { -9, -5, 6 } };
    static const int ptablePred[3][3] = { { -8, 0, 0 }, { -16, 8, 0 }, { -24, 24, -8 } };
    const int N = std::clamp (o.filterOrder, 1, 128);
    const int plen = std::clamp (o.ptableLength, 2, 64);
    auto bitOf = [&] (int ch, int i) { return (dsd[(size_t) (i >> 3) * (size_t) channels + (size_t) ch] >> (7 - (i & 7))) & 1; };

    // segmentation: one segment, or two halves (resolution 16 bytes, first segment 147 × 16 bytes)
    const int resolution = 16, firstLen = 147, split = o.twoSegments ? resolution * 8 * firstLen : bitsPerChannel;
    const int segs = o.twoSegments ? 2 : 1;
    std::vector<std::array<int, 2>> table4 ((size_t) channels);
    int numTables = 0;
    for (int ch = 0; ch < channels; ++ch)
        for (int s = 0; s < segs; ++s)
            table4[(size_t) ch][(size_t) s] = o.perChannelTables ? numTables++ : s;
    if (! o.perChannelTables) numTables = segs;
    auto tableAt = [&] (int ch, int i) { return table4[(size_t) ch][i < split ? 0 : 1]; };

    // filters: autocorrelation of the ±1 signal over each table's bits, Levinson-Durbin, scale 128
    std::vector<std::vector<int>> coef ((size_t) numTables, std::vector<int> ((size_t) N, 0));
    for (int t = 0; t < numTables; ++t)
    {
        std::vector<double> r ((size_t) N + 1, 0.0);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = N; i < bitsPerChannel; ++i)
            {
                if (tableAt (ch, i) != t) continue;
                const double x = bitOf (ch, i) * 2.0 - 1.0;
                for (int k = 0; k <= N; ++k) r[(size_t) k] += x * (bitOf (ch, i - k) * 2.0 - 1.0);
            }
        if (r[0] <= 0.0) continue;
        r[0] *= 1.0001;
        std::vector<double> a ((size_t) N + 1, 0.0), tmp;
        double err = r[0];
        for (int m = 1; m <= N; ++m)
        {
            double acc = r[(size_t) m];
            for (int k = 1; k < m; ++k) acc -= a[(size_t) k] * r[(size_t) (m - k)];
            const double kref = acc / err;
            tmp = a;
            a[(size_t) m] = kref;
            for (int k = 1; k < m; ++k) a[(size_t) k] = tmp[(size_t) k] - kref * tmp[(size_t) (m - k)];
            err *= (1.0 - kref * kref);
            if (err <= 1e-9) break;
        }
        for (int k = 0; k < N; ++k) coef[(size_t) t][(size_t) k] = std::clamp ((int) std::lround (a[(size_t) k + 1] * 128.0), -256, 255);
    }

    // look-up tables exactly as the decoder builds them
    std::vector<int16_t> tabs ((size_t) numTables * 16 * 256);
    for (int f = 0; f < numTables; ++f)
        for (int t = 0; t < 16; ++t)
            for (int v = 0; v < 256; ++v)
            {
                int sum = 0;
                const int taps = std::clamp (N - t * 8, 0, 8);
                for (int j = 0; j < taps; ++j) sum += (((v >> j) & 1) * 2 - 1) * coef[(size_t) f][(size_t) (t * 8 + j)];
                tabs[((size_t) f * 16 + (size_t) t) * 256 + (size_t) v] = (int16_t) sum;
            }

    // pass 1: predictions and residuals; Ptable statistics per table and index
    std::vector<int16_t> pred ((size_t) channels * bitsPerChannel);
    std::vector<uint8_t> res ((size_t) channels * bitsPerChannel);
    std::vector<std::array<double, 64>> n0 ((size_t) numTables), n ((size_t) numTables);
    for (auto& x : n0) x.fill (0.0);
    for (auto& x : n) x.fill (0.0);
    std::vector<uint64_t> lo ((size_t) channels, 0xAAAAAAAAAAAAAAAAull), hi ((size_t) channels, 0xAAAAAAAAAAAAAAAAull);
    for (int i = 0; i < bitsPerChannel; ++i)
        for (int ch = 0; ch < channels; ++ch)
        {
            const int16_t* tab = tabs.data() + (size_t) tableAt (ch, i) * 16 * 256;
            int sum = 0;
            for (int t = 0; t < 8; ++t) sum += tab[t * 256 + (int) ((lo[(size_t) ch] >> (8 * t)) & 0xFF)];
            for (int t = 0; t < 8; ++t) sum += tab[(t + 8) * 256 + (int) ((hi[(size_t) ch] >> (8 * t)) & 0xFF)];
            const auto p = (int16_t) sum;
            const int v = bitOf (ch, i);
            const auto idx = (size_t) ch * bitsPerChannel + (size_t) i;
            pred[idx] = p;
            res[idx] = (uint8_t) ((((uint16_t) p >> 15) ^ (unsigned) v) & 1);
            const bool half = o.halfProbOddChannels && (ch & 1) && i < N;
            if (! half)
            {
                const int k = std::min ((p < 0 ? -(int) p : (int) p) >> 3, plen - 1);
                n[(size_t) tableAt (ch, i)][(size_t) k] += 1.0;
                if (res[idx] == 0) n0[(size_t) tableAt (ch, i)][(size_t) k] += 1.0;
            }
            hi[(size_t) ch] = (hi[(size_t) ch] << 1) | (lo[(size_t) ch] >> 63);
            lo[(size_t) ch] = (lo[(size_t) ch] << 1) | (uint64_t) v;
        }
    std::vector<std::vector<int>> ptab ((size_t) numTables, std::vector<int> ((size_t) plen, 128));
    for (int t = 0; t < numTables; ++t)
        for (int k = 0; k < plen; ++k)
            ptab[(size_t) t][(size_t) k] = std::clamp ((int) std::lround (256.0 * (n0[(size_t) t][(size_t) k] + 0.4) / (n[(size_t) t][(size_t) k] + 0.8)), 1, 128);

    // header
    BitWriter w;
    w.put (1, 1);                                   // DST_Coded
    w.put (1, 1);                                   // Ptables segmented as the filters
    w.put (1, 1);                                   // same segmentation for all channels
    if (o.twoSegments)
    {
        const int maxRes = frameBytesPerChannel - 1024 / 8;
        w.put (1, 0);                               // not the end of the channel: one coded segment
        w.put (bitsFor (maxRes), (unsigned) resolution);
        w.put (bitsFor (maxRes / resolution), (unsigned) firstLen);
    }
    w.put (1, 1);                                   // end of channel
    w.put (1, 1);                                   // Ptables mapped as the filters
    w.put (1, o.perChannelTables ? 0u : 1u);        // same mapping for all channels
    {
        int count = 1;
        auto putTable = [&] (int t) { w.put (bitsFor (count), (unsigned) t); if (t == count) ++count; };
        if (o.perChannelTables)
        {
            for (int ch = 0; ch < channels; ++ch)
                for (int s = 0; s < segs; ++s)
                    if (ch != 0 || s != 0) putTable (table4[(size_t) ch][(size_t) s]);
        }
        else
            for (int s = 1; s < segs; ++s) putTable (s);
    }
    for (int ch = 0; ch < channels; ++ch) w.put (1, o.halfProbOddChannels && (ch & 1) ? 1u : 0u);
    for (int t = 0; t < numTables; ++t)
    {
        w.put (7, (unsigned) (N - 1));
        writeTable (w, coef[(size_t) t], 9, true, 0, filterPred, 6, o.allowRice);
    }
    for (int t = 0; t < numTables; ++t)
    {
        w.put (6, (unsigned) (plen - 1));
        writeTable (w, ptab[(size_t) t], 7, false, 1, ptablePred, 4, o.allowRice);
    }

    // arithmetic code: a leading 0, then the code (inverse of the decoder's interval split)
    std::vector<uint8_t> code { 0 };
    unsigned A = 4095, low = 0;
    auto carry = [&]
    {
        for (size_t k = code.size(); k-- > 0;)
        {
            if (code[k] == 1) code[k] = 0;
            else { code[k] = 1; break; }
        }
    };
    auto encode = [&] (unsigned bit, unsigned p)
    {
        const unsigned ap = ((A >> 8) | ((A >> 7) & 1)) * p;
        const unsigned h = A - ap;
        if (bit == 0) { low += h; A = ap; if (low >= 4096) { low -= 4096; carry(); } }
        else A = h;
        while (A < 2048)
        {
            A <<= 1;
            code.push_back ((uint8_t) ((low >> 11) & 1));
            low = (low << 1) & 4095;
        }
    };
    encode (0, (unsigned) reverse7Plus1 (coef[0][0]));
    for (int i = 0; i < bitsPerChannel; ++i)
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto idx = (size_t) ch * bitsPerChannel + (size_t) i;
            const int t = tableAt (ch, i);
            const bool half = o.halfProbOddChannels && (ch & 1) && i < N;
            const int k = std::min ((pred[idx] < 0 ? -(int) pred[idx] : (int) pred[idx]) >> 3, plen - 1);
            encode (res[idx], half ? 128u : (unsigned) ptab[(size_t) t][(size_t) k]);
        }
    for (int k = 11; k >= 0; --k) code.push_back ((uint8_t) ((low >> k) & 1));
    w.bits.insert (w.bits.end(), code.begin(), code.end());
    return w.bytes();
}

/** A DSD64-like test signal: 2nd-order sigma-delta of a sine (one frequency per channel), MSB first, byte-interleaved. */
inline std::vector<uint8_t> sigmaDelta (int channels, int frames, double amplitude = 0.5, unsigned seed = 1)
{
    const int bits = frames * bitsPerChannel;
    std::vector<uint8_t> out ((size_t) bits / 8 * (size_t) channels, 0);
    for (int ch = 0; ch < channels; ++ch)
    {
        double i1 = 0.0, i2 = 0.0, y = 0.0;
        const double f = (440.0 + 110.0 * ch + seed) / 2822400.0;
        uint32_t rng = 0x12345u + seed * 7919u + (unsigned) ch;
        for (int n = 0; n < bits; ++n)
        {
            rng = rng * 1664525u + 1013904223u;
            const double dither = ((rng >> 8) / 16777216.0 - 0.5) * 1e-3;
            const double x = amplitude * std::sin (6.283185307179586 * f * n) + dither;
            i1 += x - y;
            i2 += i1 - y;
            y = i2 >= 0.0 ? 1.0 : -1.0;
            if (y > 0) out[(size_t) (n >> 3) * (size_t) channels + (size_t) ch] |= (uint8_t) (1 << (7 - (n & 7)));
        }
    }
    return out;
}

} // namespace dsttest
