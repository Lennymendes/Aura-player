// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "PcmToDsd.h"
#include "FilterDesign.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace nx::dsp
{

namespace
{
    // Zeros of the noise transfer function, as fractions of the band edge: roots of the Legendre polynomial of the
    // modulator's order (they minimise the integrated in-band noise).
    const std::vector<double>& legendreRoots (int order)
    {
        static const std::vector<double> five  { 0.0, 0.5384693101, 0.9061798459 };
        static const std::vector<double> seven { 0.0, 0.4058451514, 0.7415311856, 0.9491079123 };
        return order == 5 ? five : seven;            // 0 = the real zero, the others are +/- pairs
    }
}

void PcmToDsd::prepare (double inRate, double dsdRate, int numChannels, int ord, double headroomDb)
{
    channels = std::clamp (numChannels, 1, 8);
    ratio = std::max (8, (int) std::lround (dsdRate / inRate));
    order = ord <= 5 ? 5 : 7;
    gain = 0.5 * std::pow (10.0, std::min (0.0, headroomDb) / 20.0);
    lookahead = std::max (16, (int) std::lround (inRate * 0.0007));         // 0.7 ms

    // --- noise transfer function: zeros in 0-20 kHz, Butterworth high-pass poles, |NTF| max = 1.5 --------------
    const double wb = 2.0 * kPi * 20000.0 / dsdRate;
    const auto& roots = legendreRoots (order);
    auto polesFor = [this] (double wc)
    {
        // analogue Butterworth low-pass poles -> high-pass (s -> wa / s) -> bilinear (z = (2 + s) / (2 - s))
        const double wa = 2.0 * std::tan (wc / 2.0);
        std::vector<std::complex<double>> p;
        for (int k = 0; k < order; ++k)
        {
            const auto lp = std::polar (1.0, kPi * (2.0 * k + order + 1.0) / (2.0 * order));
            const auto hp = wa / lp;
            p.push_back ((2.0 + hp) / (2.0 - hp));
        }
        return p;
    };
    auto build = [&] (double wc)
    {
        auto p = polesFor (wc);
        std::vector<std::complex<double>> upper;     // one pole of each conjugate pair, by angle
        double realPole = 0.0;
        for (auto& z : p)
        {
            if (std::abs (z.imag()) < 1e-12) realPole = z.real();
            else if (z.imag() > 0) upper.push_back (z);
        }
        std::sort (upper.begin(), upper.end(), [] (auto a, auto b) { return std::arg (a) < std::arg (b); });
        sections.clear();
        sections.push_back ({ -1.0, 0.0, -realPole, 0.0 });                     // (1 - z^-1) / (1 - p z^-1)
        for (size_t i = 0; i < upper.size(); ++i)
        {
            const double theta = wb * roots[i + 1];
            sections.push_back ({ -2.0 * std::cos (theta), 1.0, -2.0 * upper[i].real(), std::norm (upper[i]) });
        }
    };
    auto peak = [this]
    {
        double m = 0.0;
        for (int i = 1; i <= 4096; ++i) m = std::max (m, ntfMagnitude (kPi * i / 4096.0));
        return m;
    };
    double lo = 1e-5, hi = kPi * 0.99;
    for (int it = 0; it < 60; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        build (mid);
        (peak() > 1.5 ? hi : lo) = mid;
    }
    build (lo);

    // --- interpolation filter DXD -> DSD: cut-off at the DXD Nyquist, images (> 330 kHz) 90 dB down -----------
    const auto h = designLowpass (taps * ratio, 0.5 / ratio, 90.0, (double) ratio);
    coeffs.assign ((size_t) taps * (size_t) ratio, 0.0);
    for (int r = 0; r < ratio; ++r)
        for (int j = 0; j < taps; ++j)
            coeffs[(size_t) r * (size_t) taps + (size_t) j] = h[(size_t) (j * ratio + r)];

    history.assign ((size_t) channels * 2 * (size_t) taps, 0.0);
    historyPos.assign ((size_t) channels, 0);
    state.assign ((size_t) channels * sections.size() * 2, 0.0);
    delayLine.assign ((size_t) lookahead * (size_t) channels, 0.0);
    holdValue.assign ((size_t) lookahead + 2, 1.0);
    holdIndex.assign ((size_t) lookahead + 2, 0);
    boxRing.assign ((size_t) lookahead, 1.0);
    upsampled.assign ((size_t) channels * (size_t) ratio, 0.0);
    reset();
}

void PcmToDsd::reset() noexcept
{
    std::fill (history.begin(), history.end(), 0.0);
    std::fill (historyPos.begin(), historyPos.end(), 0);
    std::fill (state.begin(), state.end(), 0.0);
    std::fill (delayLine.begin(), delayLine.end(), 0.0);
    std::fill (boxRing.begin(), boxRing.end(), 1.0);
    delayPos = holdHead = holdTail = boxPos = 0;
    frameCount = 0;
    boxSum = (double) lookahead;
}

double PcmToDsd::ntfMagnitude (double w) const noexcept
{
    const auto z1 = std::polar (1.0, -w), z2 = z1 * z1;
    double m = 1.0;
    for (auto& s : sections)
        m *= std::abs (1.0 + s.b1 * z1 + s.b2 * z2) / std::abs (1.0 + s.a1 * z1 + s.a2 * z2);
    return m;
}

int PcmToDsd::process (const double* in, int frames, uint8_t* out) noexcept
{
    const int bytesPerFrame = ratio / 8;
    const int nsec = (int) sections.size();
    const int cap = lookahead + 2;          // hold window: lookahead + 1 frames (the delay line holds lookahead frames)
    for (int f = 0; f < frames; ++f)
    {
        // 1-2. headroom, then the look-ahead limiter: the needed gain is held (min over the look-ahead window), then
        // smoothed by a box filter of the same length, so it is reached exactly when the peak leaves the delay line.
        const double* x = in + (size_t) f * (size_t) channels;
        double pk = 0.0;
        for (int c = 0; c < channels; ++c) pk = std::max (pk, std::abs (x[c] * gain));
        const double need = pk > ceiling ? ceiling / pk : 1.0;
        while (holdTail != holdHead && holdValue[(size_t) ((holdTail + cap - 1) % cap)] >= need)
            holdTail = (holdTail + cap - 1) % cap;                               // drop larger values from the back
        holdValue[(size_t) holdTail] = need;
        holdIndex[(size_t) holdTail] = (int) (frameCount % 0x40000000);
        holdTail = (holdTail + 1) % cap;
        const int oldest = (int) ((frameCount - lookahead) % 0x40000000);
        while (holdHead != holdTail && ((holdIndex[(size_t) holdHead] - oldest + 0x40000000) % 0x40000000) > lookahead)
            holdHead = (holdHead + 1) % cap;                                     // left the window
        const double held = holdValue[(size_t) holdHead];
        boxSum += held - boxRing[(size_t) boxPos];
        boxRing[(size_t) boxPos] = held;
        boxPos = (boxPos + 1) % lookahead;
        const double g = std::min (1.0, boxSum / lookahead);
        if (g < 0.999999) ++limitedFrames;
        ++frameCount;

        double* d = delayLine.data() + (size_t) delayPos * (size_t) channels;   // oldest frame, output now
        for (int c = 0; c < channels; ++c)
        {
            const double delayed = d[c];
            d[c] = x[c] * gain;
            // 3. interpolation: newest-first history (mirrored, no modulo in the dot product)
            auto& pos = historyPos[(size_t) c];
            pos = (pos + taps - 1) % taps;
            double* hist = history.data() + (size_t) c * 2 * (size_t) taps;
            hist[pos] = hist[pos + taps] = delayed * g;
            const double* hp = hist + pos;
            double* up = upsampled.data() + (size_t) c * (size_t) ratio;
            for (int r = 0; r < ratio; ++r)
            {
                const double* k = coeffs.data() + (size_t) r * (size_t) taps;
                double acc = 0.0;
                for (int j = 0; j < taps; ++j) acc += k[j] * hp[j];
                up[r] = acc;
            }
        }
        delayPos = (delayPos + 1) % lookahead;

        // 4. sigma-delta, error feedback: w = x + (NTF - 1) e, e = y - w  =>  Y = X + NTF E
        for (int c = 0; c < channels; ++c)
        {
            double* st = state.data() + (size_t) c * (size_t) nsec * 2;
            const double* up = upsampled.data() + (size_t) c * (size_t) ratio;
            for (int b = 0; b < bytesPerFrame; ++b)
            {
                unsigned byte = 0;
                for (int bit = 0; bit < 8; ++bit)
                {
                    double fb = 0.0;
                    for (int s = 0; s < nsec; ++s) fb += st[s * 2];
                    double w = up[b * 8 + bit] + fb;
                    if (std::abs (w) > 4.0)                                       // diverged: start the loop again
                    {
                        std::fill (st, st + nsec * 2, 0.0);
                        ++loopResets;
                        w = up[b * 8 + bit];
                    }
                    const bool one = w >= 0.0;
                    double u = (one ? 1.0 : -1.0) - w;
                    for (int s = 0; s < nsec; ++s)
                    {
                        const auto& q = sections[(size_t) s];
                        const double o = u + st[s * 2];
                        st[s * 2] = q.b1 * u - q.a1 * o + st[s * 2 + 1];
                        st[s * 2 + 1] = q.b2 * u - q.a2 * o;
                        u = o;
                    }
                    byte = (byte << 1) | (one ? 1u : 0u);
                }
                out[((size_t) f * (size_t) bytesPerFrame + (size_t) b) * (size_t) channels + (size_t) c] = (uint8_t) byte;
            }
        }
    }
    return frames * bytesPerFrame;
}

} // namespace nx::dsp
