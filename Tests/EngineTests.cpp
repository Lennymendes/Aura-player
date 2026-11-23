// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Brique 1: decoders, DSP, exact float transport, and the engine end to end through a virtual DAC.
#include "TestFramework.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "TestAudioFiles.h"
#include "../Source/Core/SpscRing.h"
#include "../Source/Core/ExactSampleEncoder.h"
#include "../Source/Audio/AudioDecoder.h"
#include "../Source/DSP/DsdToPcm.h"
#include "../Source/DSP/Resampler.h"
#include "../Source/DSP/FilterDesign.h"
#include "../Source/DSP/PcmToDsd.h"
#include "../Source/DSP/E9ds.h"
#include "../Source/DSP/TruePeak.h"
#include "../Source/DSP/SmartComfort.h"
#include "../Source/GUI/Waveform.h"
#include "../Source/Library/Library.h"
#include "../Source/Engine/DeviceProfiles.h"
#include "../Source/Audio/LoopingDecoder.h"
#include <complex>
#include "../Source/Engine/PlaybackEngine.h"
#include "../Source/Output/CaptureBackend.h"
#include "../Source/Core/AudioLog.h"
#include "../Source/Audio/SacdIso.h"
#include "SacdFixture.h"
#include "DstEncoder.h"
#include "DstGolden.h"
#include "../Source/Audio/Dst.h"
#include <cmath>
#include <set>
#include <thread>
#include <tuple>

extern thread_local bool nxtest_countAllocations;
extern std::atomic<long> nxtest_allocationCount;

using namespace nxtest;

namespace
{
    std::vector<int32_t> decodeAllInt (const juce::File& f, nx::StreamFormat* fmtOut = nullptr)
    {
        juce::String err;
        auto d = nx::openDecoder (f, err);
        if (d == nullptr) return {};
        if (fmtOut) *fmtOut = d->getFormat();
        std::vector<int32_t> out;
        std::vector<int32_t> buf (4096 * (size_t) d->getFormat().numChannels);
        while (true)
        {
            const int n = d->readInt (buf.data(), 4096);
            if (n <= 0) break;
            out.insert (out.end(), buf.begin(), buf.begin() + n * d->getFormat().numChannels);
        }
        return out;
    }

    std::vector<uint8_t> decodeAllDsd (const juce::File& f, nx::StreamFormat* fmtOut = nullptr)
    {
        juce::String err;
        auto d = nx::openDecoder (f, err);
        if (d == nullptr) return {};
        if (fmtOut) *fmtOut = d->getFormat();
        std::vector<uint8_t> out, buf (8192 * (size_t) d->getFormat().numChannels);
        while (true)
        {
            const int n = d->readDsd (buf.data(), 8192);
            if (n <= 0) break;
            out.insert (out.end(), buf.begin(), buf.begin() + n * d->getFormat().numChannels);
        }
        return out;
    }

    /** Least-squares fit of a sine at freq: returns {amplitude, residual RMS}. */
    std::pair<double, double> fitSine (const double* x, int n, int stride, double rate, double freq)
    {
        double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * freq * i / rate;
            const double s = std::sin (w), c = std::cos (w);
            ss += s * s; sc += s * c; cc += c * c; xs += x[(size_t) i * stride] * s; xc += x[(size_t) i * stride] * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
        double res = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * freq * i / rate;
            const double e = x[(size_t) i * stride] - (a * std::sin (w) + b * std::cos (w));
            res += e * e;
        }
        return { std::sqrt (a * a + b * b), std::sqrt (res / n) };
    }

    struct EngineRig
    {
        std::unique_ptr<nx::PlaybackEngine> engine;
        nx::CaptureBackend* cap = nullptr;

        explicit EngineRig (std::function<void (nx::CaptureBackend&)> configure = {})
        {
            auto c = std::make_unique<nx::CaptureBackend>();
            if (configure) configure (*c);
            cap = c.get();
            engine = std::make_unique<nx::PlaybackEngine> (std::move (c));
            auto st = engine->getSettings();
            st.transitionFades = false;          // bit-perfect from the first sample (section 120 turns the fades on)
            engine->setSettings (st);
        }

        static nx::PlayableItem item (const juce::File& f)
        {
            nx::PlayableItem i;
            i.id = f.getFileName();
            i.uri = f.getFullPathName();
            i.title = f.getFileNameWithoutExtension();
            return i;
        }

        /** Pumps blocks until the queue ends (or timeout). Waits for the decoder so nothing underruns. */
        void runToEnd (int block = 1024, double timeoutSeconds = 60.0, std::function<void (int)> perBlock = {}, size_t wantBuffered = 1u << 19)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            int blocks = 0;
            while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutSeconds * 1000.0)
            {
                engine->serviceMessageThread();
                if (engine->getState() == nx::PlaybackEngine::State::Stopped && blocks > 0) break;
                if (! cap->isRunning()) { juce::Thread::sleep (1); continue; }
                for (int w = 0; w < 4000 && engine->isDecoderBusy() && engine->getBufferedBytes() < wantBuffered; ++w)
                    juce::Thread::sleep (1);
                cap->pump (block);
                if (perBlock) perBlock (blocks);
                ++blocks;
            }
        }
    };
}

//==============================================================================
NX_TEST_SECTION (101, "Ring SPSC et encodeur exact (2^24 valeurs)")
{
    nx::SpscRing<int> ring (10);
    int in[7] = { 1, 2, 3, 4, 5, 6, 7 }, out[10] {};
    bool ok = true;
    for (int round = 0; round < 50; ++round)
    {
        ok &= ring.write (in, 7) == 7;
        ok &= ring.read (out, 7) == 7;
        for (int i = 0; i < 7; ++i) ok &= out[i] == in[i];
    }
    ok &= ring.write (in, 7) == 7 && ring.write (in, 7) == 3 && ring.writable() == 0;
    t.check (ok, "ring : bouclage, plein / vide, ordre conservé"_t);

    // Exhaustive: every 24-bit integer through each float -> int rule.
    struct Case { nx::FloatToIntRule rule; int bits; juce::String name; int exactMsb; };
    const Case cases[] = {
        { nx::FloatToIntRule::Scale2N,        24, "CoreAudio / AudioData Int24 (×2^23)"_t, 24 },
        { nx::FloatToIntRule::RoundMaxMinus1, 24, "convertisseur ASIO JUCE 24 bits (×(2^23-1))"_t, 24 },
        { nx::FloatToIntRule::TruncMaxMinus1, 32, "AudioData Int32 tronqué (×(2^31-1))"_t, 24 },
        { nx::FloatToIntRule::Scale2N,        32, "HAL 32 bits (×2^31)"_t, 24 },
    };
    for (auto& c : cases)
    {
        nx::ExactSampleEncoder enc (c.rule, c.bits);
        long bad = 0, notFullyExact = 0;
        for (int64_t k = -(1 << 23) + 1; k < (1 << 23); ++k)
        {
            const auto v = (int32_t) ((uint32_t) k << 8);
            const auto got = enc.apply (enc.encode (v));
            const auto want = (int64_t) (v >> (32 - c.bits));
            if ((got >> (c.bits - c.exactMsb)) != (want >> (c.bits - c.exactMsb))) ++bad;
            if (got != want) ++notFullyExact;
        }
        t.check (bad == 0, juce::String (c.name) + " : 24 bits de poids fort exacts pour les 16 777 215 valeurs ("
                               + juce::String (notFullyExact) + " mots 32 bits avec octet bas non nul)");
    }

    // DoP words (markers 0x05 / 0xFA + 16 bits) through the truncating 32-bit rule.
    nx::ExactSampleEncoder trunc32 (nx::FloatToIntRule::TruncMaxMinus1, 32);
    long dopBad = 0;
    for (int m = 0; m < 2; ++m)
        for (int v = 0; v < 65536; ++v)
        {
            const auto w = (int32_t) (((uint32_t) (m ? 0xFA : 0x05) << 24) | ((uint32_t) v << 8));
            if ((trunc32.apply (trunc32.encode (w)) >> 8) != (int64_t) (w >> 8)) ++dopBad;
        }
    t.check (dopBad == 0, "DoP : les 131 072 mots possibles gardent marqueur + 16 bits DSD exacts avec la règle 32 bits tronquée"_t);
}

NX_TEST_SECTION (102, "Décodeurs PCM : WAV, RF64, AIFF, AIFF-C (bit à bit)")
{
    struct WavCase { int container, valid; bool rf64, ext; const char* name; };
    const WavCase wavs[] = {
        { 8, 8, false, false, "WAV 8 bits" }, { 16, 16, false, false, "WAV 16 bits" }, { 24, 24, false, false, "WAV 24 bits" },
        { 32, 32, false, false, "WAV 32 bits entiers" }, { 32, 24, false, true, "WAV 24 dans 32 (extensible)" },
        { 24, 24, true, false, "RF64 24 bits" },
    };
    for (auto& c : wavs)
    {
        const auto pcm = randomPcm (30011, 2, c.valid, (uint32_t) c.container * 7 + (uint32_t) c.valid);
        auto f = t.tempDir.getChildFile (juce::String (c.name).replaceCharacters (" ", "_") + ".wav");
        writeWav (f, &pcm, nullptr, 2, 96000, c.container, c.valid, false, c.rf64, c.ext);
        nx::StreamFormat fmt;
        const auto back = decodeAllInt (f, &fmt);
        t.check (back == pcm && fmt.bitsPerSample == c.valid && fmt.sampleRate == 96000,
                 juce::String (c.name) + " : " + juce::String ((int) back.size()) + " échantillons identiques"_t);
    }

    // float WAV 32 / 64: exact doubles
    for (int bits : { 32, 64 })
    {
        std::vector<double> x (20000);
        juce::Random r (bits);
        for (auto& v : x) v = (bits == 32 ? (double) (float) (r.nextDouble() * 2 - 1) : r.nextDouble() * 2 - 1);
        auto f = t.tempDir.getChildFile ("float" + juce::String (bits) + ".wav");
        writeWav (f, nullptr, &x, 2, 352800, bits, bits, true);
        juce::String err;
        auto d = nx::openDecoder (f, err);
        std::vector<double> back (x.size());
        const int n = d != nullptr ? d->readDouble (back.data(), (int) x.size() / 2) : 0;
        t.check (n * 2 == (int) x.size() && back == x && d->getFormat().kind == nx::SampleKind::PcmFloat,
                 "WAV " + juce::String (bits) + " bits flottant DXD : valeurs identiques");
    }

    for (const char* comp : { "NONE", "sowt" })
        for (int bits : { 16, 24 })
        {
            const auto pcm = randomPcm (12007, 2, bits, (uint32_t) bits);
            auto f = t.tempDir.getChildFile (juce::String ("aiff_") + comp + juce::String (bits) + ".aif");
            writeAiff (f, &pcm, nullptr, 2, 192000, bits, comp);
            nx::StreamFormat fmt;
            const auto back = decodeAllInt (f, &fmt);
            t.check (back == pcm && std::abs (fmt.sampleRate - 192000) < 1e-6,
                     juce::String (juce::String (comp) == "NONE" ? "AIFF " : "AIFF-C sowt ") + juce::String (bits) + " bits / 192 kHz identique");
        }

    {
        std::vector<double> x (8000);
        juce::Random r (5);
        for (auto& v : x) v = r.nextDouble() - 0.5;
        auto f = t.tempDir.getChildFile ("aifc_fl64.aif");
        writeAiff (f, nullptr, &x, 2, 384000, 64, "fl64");
        juce::String err;
        auto d = nx::openDecoder (f, err);
        std::vector<double> back (x.size());
        const int n = d != nullptr ? d->readDouble (back.data(), 4000) : 0;
        t.check (n == 4000 && back == x, "AIFF-C fl64 384 kHz identique");
    }

    // Seek: frame-accurate
    {
        const auto pcm = randomPcm (50000, 2, 24, 99);
        auto f = t.tempDir.getChildFile ("seek.wav");
        writeWav (f, &pcm, nullptr, 2, 44100, 24, 24, false);
        juce::String err;
        auto d = nx::openDecoder (f, err);
        d->seek (31234);
        std::vector<int32_t> buf (200);
        d->readInt (buf.data(), 100);
        t.check (std::equal (buf.begin(), buf.end(), pcm.begin() + 31234 * 2), "WAV : seek à l'échantillon près"_t);
    }
}

NX_TEST_SECTION (103, "Décodeurs sans perte : FLAC et ALAC (bit à bit)")
{
    for (int bits : { 16, 24 })
    {
        const auto pcm = sinePcm (48000, 2, bits, 96000, 997, 0.8);
        auto f = t.tempDir.getChildFile ("flac" + juce::String (bits) + ".flac");
        {
            juce::FlacAudioFormat flac;
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            auto w = flac.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (96000).withNumChannels (2)
                                                   .withBitsPerSample (bits).withQualityOptionIndex (8));
            std::vector<int> l (48000), r (48000);
            for (int i = 0; i < 48000; ++i) { l[(size_t) i] = pcm[(size_t) i * 2]; r[(size_t) i] = pcm[(size_t) i * 2 + 1]; }
            const int* chans[] = { l.data(), r.data(), nullptr };
            if (w) w->write (chans, 48000);
        }
        nx::StreamFormat fmt;
        const auto back = decodeAllInt (f, &fmt);
        t.check (back == pcm && fmt.codec == "FLAC" && fmt.kind == nx::SampleKind::PcmInt,
                 "FLAC " + juce::String (bits) + " bits / 96 kHz : décodage entier identique"_t);
    }

    for (int bits : { 16, 24 })
    {
        const auto pcm = sinePcm (20000, 2, bits, 192000, 1234.5, 0.7);
        auto f = t.tempDir.getChildFile ("alac" + juce::String (bits) + ".m4a");
        writeAlacM4a (f, pcm, 2, 192000, bits);
        nx::StreamFormat fmt;
        const auto back = decodeAllInt (f, &fmt);
        t.check (back == pcm && fmt.codec == "ALAC" && fmt.bitsPerSample == bits,
                 "ALAC " + juce::String (bits) + " bits / 192 kHz (M4A) : identique");

        juce::String err;
        auto d = nx::openDecoder (f, err);
        d->seek (12345);
        std::vector<int32_t> buf (100);
        d->readInt (buf.data(), 50);
        t.check (std::equal (buf.begin(), buf.end(), pcm.begin() + 12345 * 2), "ALAC : seek au milieu d'un paquet exact");
    }
}

NX_TEST_SECTION (104, "Décodeurs DSD : DSF (LSB / MSB) et DFF")
{
    const auto dsd = dsdSine (2822400.0 * 2, 0.25, 2, 1000.0, 0.5);   // DSD128
    for (bool lsb : { true, false })
    {
        auto f = t.tempDir.getChildFile (lsb ? "dsd_lsb.dsf" : "dsd_msb.dsf");
        writeDsf (f, dsd, 2, 5644800.0, lsb);
        nx::StreamFormat fmt;
        const auto back = decodeAllDsd (f, &fmt);
        t.check (back == dsd && fmt.dsdMultiple() == 128,
                 juce::String ("DSF ") + (lsb ? "LSB-first" : "MSB-first") + " DSD128 : " + juce::String ((int) back.size()) + " octets identiques");
    }
    {
        auto f = t.tempDir.getChildFile ("dsd.dff");
        writeDff (f, dsd, 2, 5644800.0);
        nx::StreamFormat fmt;
        const auto back = decodeAllDsd (f, &fmt);
        t.check (back == dsd && fmt.codec == "DFF", "DFF DSD128 : octets identiques");

        juce::String err;
        auto d = nx::openDecoder (f, err);
        d->seek (50001);
        uint8_t buf[64];
        d->readDsd (buf, 32);
        t.check (std::equal (buf, buf + 64, dsd.begin() + 50001 * 2), "DFF : seek à l'octet près"_t);
    }
}

NX_TEST_SECTION (105, "DSP 64 bits : DSD -> DXD et rééchantillonnage sinc")
{
    for (int mult : { 64, 128 })
    {
        const double dsdRate = 44100.0 * mult;
        const auto dsd = dsdSine (dsdRate, 0.5, 1, 1000.0, 0.5);
        nx::dsp::DsdToPcm conv;
        conv.prepare (dsdRate, 352800.0, 1);
        std::vector<double> pcm ((size_t) conv.maxOutputFor ((int) dsd.size()));
        const int n = conv.process (dsd.data(), (int) dsd.size(), pcm.data(), (int) pcm.size());

        // Band-limit to 20 kHz (352.8 -> 44.1 kHz) then fit the 1 kHz sine.
        nx::dsp::Resampler down;
        down.prepare (352800.0, 44100.0, 1);
        std::vector<double> base ((size_t) down.maxOutputFor (n));
        const int m = down.process (pcm.data(), n, base.data(), (int) base.size());
        const int skip = 2000;
        const auto [amp, res] = fitSine (base.data() + skip, m - 2 * skip, 1, 44100.0, 1000.0);
        const double snr = 20.0 * std::log10 (amp / std::sqrt (2.0) / res);
        t.check (std::abs (amp - 0.5) < 0.005 && snr > (mult == 64 ? 70.0 : 85.0),
                 "DSD" + juce::String (mult) + " -> DXD 352,8 kHz : " + juce::String (conv.getNumTaps()) + " coefficients, amplitude "
                     + juce::String (amp, 4) + ", SNR 20 kHz " + juce::String (snr, 1) + " dB (modulateur d'ordre 2 du test)");
    }

    struct RCase { double in, out; };
    for (auto rc : { RCase { 44100, 352800 }, RCase { 96000, 44100 }, RCase { 44100, 48000 }, RCase { 192000, 384000 } })
    {
        nx::dsp::Resampler r;
        r.prepare (rc.in, rc.out, 1);
        const int n = (int) rc.in;   // 1 s
        std::vector<double> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rc.in);
        std::vector<double> y ((size_t) r.maxOutputFor (n));
        int m = 0;
        for (int start = 0; start < n; start += 4096)   // streaming in blocks
            m += r.process (x.data() + start, juce::jmin (4096, n - start), y.data() + m, (int) y.size() - m);
        const double lat = r.getLatencyInputSamples();
        double err = 0; int cnt = 0;
        for (int k = (int) (rc.out * 0.05); k < (int) (rc.out * 0.9); ++k)
        {
            const double tIn = (double) k * rc.in / rc.out - lat;
            const double ref = 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * tIn / rc.in);
            err += (y[(size_t) k] - ref) * (y[(size_t) k] - ref); ++cnt;
        }
        const double db = 10.0 * std::log10 (err / cnt / (0.125));
        t.check (db < -140.0, "Sinc " + nx::formatRate (rc.in) + " -> " + nx::formatRate (rc.out) + " (L/M = " + juce::String (r.getL()) + "/"
                                  + juce::String (r.getM()) + ", " + juce::String (r.getTapsPerPhase()) + " coef./phase) : erreur " + juce::String (db, 1) + " dB");
    }
}

NX_TEST_SECTION (106, "Moteur : bit-perfect, gapless, changement de format")
{
    // Three 24/96 tracks with random data: the virtual DAC must receive their exact concatenation.
    std::vector<int32_t> all;
    std::vector<nx::PlayableItem> items;
    for (int k = 0; k < 3; ++k)
    {
        const auto pcm = randomPcm (40000 + 1234 * k, 2, 24, 500u + (uint32_t) k);
        auto f = t.tempDir.getChildFile ("gapless" + juce::String (k) + ".wav");
        writeWav (f, &pcm, nullptr, 2, 96000, 24, 24, false);
        all.insert (all.end(), pcm.begin(), pcm.end());
        items.push_back (EngineRig::item (f));
    }

    {
        EngineRig rig;
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        int trackChanges = 0;
        rig.engine->onTrackChanged = [&] { ++trackChanges; };
        rig.engine->setQueue (items, 0, true);
        rig.runToEnd();

        const auto& got = rig.cap->capturedInt;
        size_t firstDiff = all.size();
        for (size_t i = 0; i < all.size(); ++i)
            if (i >= got.size() || got[i] != all[i]) { firstDiff = i; break; }
        bool tailSilent = true;
        for (size_t i = all.size(); i < got.size(); ++i) tailSilent &= got[i] == 0;

        t.check (firstDiff == all.size(), "3 pistes 24/96 enchaînées : "_t + juce::String ((int) all.size() / 2)
                                              + " trames identiques, sans trou ni recouvrement"
                                              + (firstDiff < all.size() ? " (écart à "_t + juce::String ((int) firstDiff) + ")" : juce::String()));
        t.check (tailSilent, "silence numérique après la fin de la file"_t);
        t.check (rig.cap->opens == 1, "un seul (ré)ouverture du DAC pour un album homogène (gapless)"_t);
        t.check (rig.engine->getStats().underruns.load() == 0, "aucun sous-débit"_t);
        t.check (nxtest_allocationCount.load() == 0, "0 allocation sur le thread audio (" + juce::String ((int) rig.engine->getStats().callbacks.load()) + " blocs)");
        t.check (trackChanges >= 3, "changements de piste signalés : "_t + juce::String (trackChanges));
        t.check (rig.cap->config.sampleRate == 96000, "DAC commuté automatiquement à 96 kHz"_t);
    }

    // Format change 44.1/16 -> 96/24: the DAC is reopened at the boundary, both tracks stay exact.
    {
        const auto a = randomPcm (30000, 2, 16, 1);
        const auto b = randomPcm (20000, 2, 24, 2);
        auto fa = t.tempDir.getChildFile ("cd.wav"), fb = t.tempDir.getChildFile ("hires.wav");
        writeWav (fa, &a, nullptr, 2, 44100, 16, 16, false);
        writeWav (fb, &b, nullptr, 2, 96000, 24, 24, false);

        EngineRig rig;
        std::vector<double> ratesSeen;
        rig.engine->setQueue ({ EngineRig::item (fa), EngineRig::item (fb) }, 0, true);
        rig.runToEnd (1024, 60.0, [&] (int) { if (ratesSeen.empty() || ratesSeen.back() != rig.cap->config.sampleRate) ratesSeen.push_back (rig.cap->config.sampleRate); });

        std::vector<int32_t> expected (a);
        expected.insert (expected.end(), b.begin(), b.end());
        auto got = rig.cap->capturedInt;
        // Remove the silence the DAC played while it was being reconfigured.
        std::vector<int32_t> nonSilentStart;
        bool ok = got.size() >= expected.size();
        size_t gi = 0;
        for (size_t i = 0; ok && i < a.size(); ++i) ok = got[gi++] == a[i];
        while (ok && gi + 1 < got.size() && got[gi] == 0 && got[gi + 1] == 0 && b[0] != 0) gi += 2;
        for (size_t i = 0; ok && i < b.size(); ++i) ok = gi < got.size() && got[gi++] == b[i];
        t.check (ok, "44,1 kHz / 16 bits puis 96 kHz / 24 bits : les deux pistes arrivent intactes");
        t.check (rig.cap->opens == 2 && ratesSeen.size() >= 2, "DAC rouvert à la frontière (44,1 -> 96 kHz)"_t);
    }

    // Mono 24/48 -> both channels, still bit-perfect
    {
        const auto m = randomPcm (10000, 1, 24, 77);
        auto f = t.tempDir.getChildFile ("mono.wav");
        writeWav (f, &m, nullptr, 1, 48000, 24, 24, false);
        EngineRig rig;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd();
        bool ok = rig.cap->capturedInt.size() >= m.size() * 2;
        for (size_t i = 0; ok && i < m.size(); ++i) ok = rig.cap->capturedInt[i * 2] == m[i] && rig.cap->capturedInt[i * 2 + 1] == m[i];
        t.check (ok, "mono -> deux voies, bit-perfect");
    }
}

NX_TEST_SECTION (107, "Moteur : DoP, DSD natif, DSD -> PCM")
{
    const auto dsd = dsdSine (2822400.0, 0.4, 2, 1000.0, 0.5);
    auto f = t.tempDir.getChildFile ("engine.dsf");
    writeDsf (f, dsd, 2, 2822400.0, true);

    // DoP at 176.4 kHz
    {
        EngineRig rig;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd();
        const auto& got = rig.cap->capturedInt;
        bool markersOk = true, payloadOk = true;
        // The stream starts with the DoP pre-roll (true DSD silence 0x69, >= 400 ms): skip it, then the music.
        size_t pre = 0;
        while (pre < got.size() / 2 && ((uint32_t) got[pre * 2] & 0xFFFF00) == 0x696900 && ((uint32_t) got[pre * 2 + 1] & 0xFFFF00) == 0x696900) ++pre;
        const size_t avail = got.size() / 2 > pre ? got.size() / 2 - pre : 0;
        size_t frames = juce::jmin (avail, dsd.size() / 4);
        uint8_t lastMarker = 0;
        for (size_t i = 0; i < frames; ++i)
        {
            const auto wl = (uint32_t) got[(pre + i) * 2], wr = (uint32_t) got[(pre + i) * 2 + 1];
            const uint8_t ml = (uint8_t) (wl >> 24), mr = (uint8_t) (wr >> 24);
            markersOk &= (ml == 0x05 || ml == 0xFA) && ml == mr && ml != lastMarker && (wl & 0xFF) == 0;
            lastMarker = ml;
            payloadOk &= (uint8_t) (wl >> 16) == dsd[i * 4] && (uint8_t) (wl >> 8) == dsd[i * 4 + 2]
                      && (uint8_t) (wr >> 16) == dsd[i * 4 + 1] && (uint8_t) (wr >> 8) == dsd[i * 4 + 3];
        }
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->config.sampleRate == 176400.0,
                 "DSD64 -> DoP à 176,4 kHz"_t);
        t.check (markersOk, "marqueurs DoP 0x05 / 0xFA alternés sur "_t + juce::String ((int) frames) + " trames");
        t.check (pre >= 70560, "pré-roll DoP : au moins 400 ms de silence DSD (0x69) avant la musique"_t);
        t.check (payloadOk, "charge DSD intacte dans les mots DoP");
        t.check (rig.engine->getSignalPath().overall == nx::PathQuality::BitPerfect, "Signal Path : bit-perfect");
    }

    // Native DSD
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0 }; });
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (512);
        const auto& got = rig.cap->capturedDsd;
        const bool ok = got.size() >= dsd.size() && std::equal (dsd.begin(), dsd.end(), got.begin());
        t.check (rig.cap->config.wire == nx::WireFormat::NativeDsd && ok, "DSD natif : " + juce::String ((int) dsd.size()) + " octets identiques");
    }

    // DSD -> PCM when the DAC only takes 44.1 / 88.2 / 352.8 kHz and DoP is off
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 44100.0, 88200.0, 352800.0 }; });
        auto s = rig.engine->getSettings();
        s.dopAllowed = false;
        rig.engine->setSettings (s);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd();
        const auto& got = rig.cap->capturedInt;
        std::vector<double> l (got.size() / 2);
        for (size_t i = 0; i < l.size(); ++i) l[i] = got[i * 2] / 2147483648.0;
        const int skip = 20000;
        const auto fit = l.size() > (size_t) (3 * skip) ? fitSine (l.data() + skip, (int) l.size() - 3 * skip, 1, 352800.0, 1000.0)
                                                         : std::pair<double, double> { 0, 1 };
        t.check (rig.cap->config.sampleRate == 352800.0 && std::abs (fit.first - 0.5) < 0.01,
                 "DSD64 -> PCM 352,8 kHz : amplitude " + juce::String (fit.first, 4) + " (SACD 0 dB = −6 dBFS)"_t);
        t.check (rig.engine->getCurrentPlan().conversion == nx::PlaybackPlan::Conversion::DsdToPcm,
                 "plan : conversion DSD -> PCM choisie quand ni DoP ni DSD natif");
    }
}

NX_TEST_SECTION (108, "Moteur : seek, volume, suréchantillonnage, Signal Path")
{
    const auto pcm = randomPcm (96000 * 3, 2, 24, 4242);
    auto f = t.tempDir.getChildFile ("seek_engine.wav");
    writeWav (f, &pcm, nullptr, 2, 96000, 24, 24, false);

    {
        EngineRig rig;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        bool seeked = false;
        size_t mark = 0;
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 20 && ! seeked)
            {
                rig.engine->seek (2.0);
                seeked = true;
                mark = rig.cap->capturedInt.size();
            }
        });
        const auto& got = rig.cap->capturedInt;
        // After the seek, find the first non-silent frame and compare with the source at 2.0 s.
        size_t i = mark;
        while (i + 1 < got.size() && got[i] == 0 && got[i + 1] == 0) i += 2;
        const size_t srcStart = 192000 * 2;
        bool ok = got.size() - i >= 1000;
        for (size_t k = 0; ok && k < 1000; ++k) ok = got[i + k] == pcm[srcStart + k];
        t.check (ok, "seek à 2,000 s : reprise exacte à l'échantillon 192 000"_t);
    }

    {
        EngineRig rig;
        rig.engine->setFixedVolume (false);
        rig.engine->setVolume (0.5);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        nx::SignalPath path;
        rig.runToEnd (1024, 60.0, [&] (int block) { if (block == 10) path = rig.engine->getSignalPath(); });
        const auto& got = rig.cap->capturedInt;
        double maxErr = 0;
        for (size_t i = 48000; i < 96000 && i < got.size(); ++i)
            maxErr = juce::jmax (maxErr, std::abs ((double) (got[i] >> 8) - (double) (pcm[i] >> 8) * 0.5));
        t.check (maxErr <= 2.0, "volume numérique −6,02 dB : erreur max "_t + juce::String (maxErr, 2) + " LSB (dither TPDF)");
        t.check (path.overall == nx::PathQuality::HighQuality, "Signal Path : volume -> « Haute qualité » (plus bit-perfect)"_t);
    }

    {
        const auto s = sinePcm (44100, 2, 24, 44100, 1000.0, 0.5);
        auto f2 = t.tempDir.getChildFile ("up.wav");
        writeWav (f2, &s, nullptr, 2, 44100, 24, 24, false);
        EngineRig rig;
        auto st = rig.engine->getSettings();
        st.upsampleToDxd = true;
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ EngineRig::item (f2) }, 0, true);
        nx::SignalPath path;
        rig.runToEnd (1024, 60.0, [&] (int block) { if (block == 10) path = rig.engine->getSignalPath(); });
        const auto& got = rig.cap->capturedInt;
        std::vector<double> l (got.size() / 2);
        for (size_t i = 0; i < l.size(); ++i) l[i] = got[i * 2] / 2147483648.0;
        const int skip = 30000;
        const auto fit = l.size() > (size_t) (3 * skip) ? fitSine (l.data() + skip, (int) l.size() - 3 * skip, 1, 352800.0, 1000.0)
                                                         : std::pair<double, double> { 0, 1 };
        const double snr = fit.second > 0 ? 20 * std::log10 (fit.first / std::sqrt (2.0) / fit.second) : 0;
        t.check (rig.cap->config.sampleRate == 352800.0 && snr > 130.0,
                 "sinc 44,1 -> DXD 352,8 kHz : SNR " + juce::String (snr, 1) + " dB (limité par la quantification 24 bits)"_t);
        t.check (path.overall == nx::PathQuality::Enhanced, "Signal Path : « Amélioré »"_t);
    }

    {
        EngineRig rig;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        for (int i = 0; i < 200 && ! rig.cap->isRunning(); ++i) { rig.engine->serviceMessageThread(); juce::Thread::sleep (2); }
        const auto path = rig.engine->getSignalPath();
        juce::String chain;
        for (auto& n : path.nodes) chain << n.stage << " [" << n.title << "] ";
        t.check (path.overall == nx::PathQuality::BitPerfect && path.nodes.size() >= 3, "Signal Path bit-perfect : " + chain);
    }
}

//==============================================================================
NX_TEST_SECTION (109, "Profils matériel : Merging Anubis / RAVENNA (DXD, phase minimale, DoP Sync Safe)")
{
    // Detection
    t.check (nx::isMergingDevice ("Merging RAVENNA/AES67") && nx::isMergingDevice ("MT VAD") && nx::isMergingDevice ("ANUBIS SPS 0x1234")
             && ! nx::isMergingDevice ("RME ADI-2 Pro") && ! nx::isMergingDevice ("FiiO KA17") && ! nx::isMergingDevice ("Invader Speakers"),
             "détection : Merging / Anubis / RAVENNA / VAD, pas les autres DAC"_t);

    // Plans for each profile on an Anubis-like device (PCM up to 384 kHz, DoP-capable)
    nx::DeviceCapabilities caps;
    caps.sampleRates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0 };
    caps.maxBits = 24;
    nx::StreamFormat dsd128; dsd128.kind = nx::SampleKind::Dsd; dsd128.sampleRate = 5644800.0; dsd128.bitsPerSample = 1; dsd128.codec = "DSF";
    nx::StreamFormat dsd512 = dsd128; dsd512.sampleRate = 22579200.0;
    nx::StreamFormat pcm441; pcm441.sampleRate = 44100.0; pcm441.bitsPerSample = 16; pcm441.codec = "FLAC";
    nx::EngineSettings base;
    const juce::String anubis = "Merging RAVENNA/AES67";

    auto ref = nx::resolveProfile (base, anubis);
    const auto pRefDsd = nx::makePlan (dsd128, caps, ref), pRefDsd512 = nx::makePlan (dsd512, caps, ref), pRefPcm = nx::makePlan (pcm441, caps, ref);
    t.check (pRefDsd.wire == nx::WireFormat::Pcm && pRefDsd.conversion == nx::PlaybackPlan::Conversion::DsdToPcm && pRefDsd.deviceRate == 352800.0
             && pRefDsd512.deviceRate == 352800.0 && ref.dsdToPcmGainDb == 6.0 && ref.limiter,
             "Studio DXD Reference : DSD128 / DSD512 -> DXD 352,8 kHz, +6 dB + limiteur, aucun DoP"_t);
    t.check (pRefPcm.conversion == nx::PlaybackPlan::Conversion::None && pRefPcm.deviceRate == 44100.0,
             "Studio DXD Reference : PCM 44,1 kHz bit-perfect, sans rééchantillonnage"_t);
    t.check (nx::pipelineLabel (pRefDsd, "Amélioré"_t) == juce::String::fromUTF8 ("DSD128 \xe2\x86\x92 DXD 352,8 kHz [Anubis Ref]"),
             "badge : "_t + nx::pipelineLabel (pRefDsd, "Amélioré"_t));

    base.anubisProfile = nx::AnubisProfile::AnalogWarmth;
    auto warm = nx::resolveProfile (base, anubis);
    const auto pWarmPcm = nx::makePlan (pcm441, caps, warm);
    t.check (pWarmPcm.conversion == nx::PlaybackPlan::Conversion::Resample && pWarmPcm.deviceRate == 352800.0
             && pWarmPcm.filterShape == nx::FilterShape::MinimumPhase && warm.warmth,
             "Analog Warmth : PCM 44,1 -> DXD 352,8 kHz, phase minimale, modelage harmonique"_t);
    t.check (nx::pipelineLabel (pWarmPcm, "Amélioré"_t) == juce::String::fromUTF8 ("PCM 44,1 \xe2\x86\x92 DXD [Warm]"),
             "badge : "_t + nx::pipelineLabel (pWarmPcm, "Amélioré"_t));

    base.anubisProfile = nx::AnubisProfile::PureDoP;
    auto dop = nx::resolveProfile (base, anubis);
    nx::StreamFormat dsd64 = dsd128; dsd64.sampleRate = 2822400.0;
    const auto pDop = nx::makePlan (dsd64, caps, dop);
    t.check (pDop.wire == nx::WireFormat::DoP && pDop.deviceRate == 176400.0 && dop.bufferFrames == 2048 && dop.maxChannels == 2 && dop.dopPrerollMs == 1500
             && nx::pipelineLabel (pDop, {}) == "DSD64 DoP [Bit-Perfect]",
             "Pure DoP : DoP 176,4 kHz, tampon 2048, 2 canaux, 1,5 s de silence DoP"_t);

    base.anubisProfile = nx::AnubisProfile::StudioDxdReference;
    t.check (nx::resolveProfile (base, "Topping D90").profileTag.isEmpty()
             && nx::makePlan (dsd64, caps, nx::resolveProfile (base, "Topping D90")).wire == nx::WireFormat::DoP,
             "autres DAC : réglages génériques inchangés (DoP)"_t);

    // RME ADI-2 (USB)
    const juce::String rme = "ADI-2 Pro (72241234)";
    t.check (nx::isRmeDevice (rme) && nx::isRmeDevice ("RME ADI-2 DAC") && ! nx::isRmeDevice ("Merging RAVENNA") && ! nx::isRmeDevice ("Thermostat")
             && nx::activeFamily (base, rme) == nx::DeviceFamily::Rme, "détection RME / ADI-2"_t);
    auto rmeDop = nx::resolveProfile (base, rme);
    const auto pRme = nx::makePlan (dsd128, caps, rmeDop);
    t.check (pRme.wire == nx::WireFormat::DoP && pRme.deviceRate == 352800.0 && rmeDop.dopPrerollMs == 150 && rmeDop.lockVolume
             && nx::makePlan (pcm441, caps, rmeDop).conversion == nx::PlaybackPlan::Conversion::None
             && nx::pipelineLabel (pRme, {}) == "DSD128 DoP [RME Bit-Perfect]",
             "RME Horloge DoP Haute Precision : DSD128 en DoP 352,8 kHz, synchro 150 ms, volume verrouillé, PCM bit-perfect"_t);
    base.rmeProfile = nx::RmeProfile::VelvetDxd;
    auto velvet = nx::resolveProfile (base, rme);
    const auto pVelvet = nx::makePlan (dsd128, caps, velvet);
    t.check (pVelvet.conversion == nx::PlaybackPlan::Conversion::DsdToPcm && pVelvet.deviceRate == 352800.0 && pVelvet.filterShape == nx::FilterShape::MinimumPhase
             && velvet.dsdToPcmGainDb == 6.0 && ! velvet.lockVolume
             && nx::pipelineLabel (pVelvet, {}) == juce::String::fromUTF8 ("DSD128 \xe2\x86\x92 DXD 352,8 kHz [RME Velvet]"),
             "RME AKM Velvet DXD : DSD -> DXD 352,8 kHz +6 dB, phase minimale"_t);
    base.rmeProfile = nx::RmeProfile::HighPrecisionDoP;

    // Minimum-phase filter: same magnitude, energy at the start (no pre-ringing)
    {
        const auto lin = nx::dsp::designLowpass (255, 0.2, 150.0);
        const auto mp = nx::dsp::toMinimumPhase (lin);
        auto magDb = [] (const std::vector<double>& h, double f)
        {
            std::complex<double> acc;
            for (size_t n = 0; n < h.size(); ++n) acc += h[n] * std::polar (1.0, -2.0 * nx::dsp::kPi * f * (double) n);
            return 20.0 * std::log10 (std::max (std::abs (acc), 1.0e-20));
        };
        double maxPassErr = 0.0, worstStop = -1000.0;
        for (double f = 0.0; f < 0.15; f += 0.005) maxPassErr = std::max (maxPassErr, std::abs (magDb (mp, f) - magDb (lin, f)));
        for (double f = 0.26; f < 0.5; f += 0.003) worstStop = std::max (worstStop, magDb (mp, f));
        double total = 0.0, early = 0.0;
        for (size_t n = 0; n < mp.size(); ++n) { total += mp[n] * mp[n]; if (n < mp.size() / 4) early += mp[n] * mp[n]; }
        double linEarly = 0.0;
        for (size_t n = 0; n < lin.size() / 4; ++n) linEarly += lin[n] * lin[n];
        t.check (maxPassErr < 0.001 && worstStop < -120.0 && early / total > 0.97 && linEarly / total < 0.01,
                 "phase minimale : bande passante identique ("_t + juce::String (maxPassErr, 5) + " dB), réjection "
                 + juce::String (-worstStop, 0) + " dB, " + juce::String (100.0 * early / total, 2) + " % de l'énergie au début"_t);
    }

    // The whole engine on a virtual Anubis: DSD64 at −6 dBFS -> DXD with +6 dB, limited, never clipped
    const auto dsd = dsdSine (2822400.0, 0.4, 2, 1000.0, 0.5);
    auto f = t.tempDir.getChildFile ("anubis109.dsf");
    writeDsf (f, dsd, 2, 2822400.0, true);
    {
        EngineRig rig ([&] (nx::CaptureBackend& c) { c.name = anubis; });
        rig.engine->setDevice (anubis);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        bool profileNode = false;
        rig.runToEnd (1024, 60.0, [&] (int)
        {
            for (auto& n : rig.engine->getSignalPath().nodes) profileNode |= n.stage == "Profil" && n.title.contains ("Studio DXD Reference");
        });
        const auto& got = rig.cap->capturedInt;
        double peak = 0.0;
        for (auto v : got) peak = std::max (peak, std::abs (v / 2147483648.0));
        std::vector<double> l (got.size() / 2);
        for (size_t i = 0; i < l.size(); ++i) l[i] = got[i * 2] / 2147483648.0;
        const int skip = 20000;
        const auto fit = l.size() > (size_t) (3 * skip) ? fitSine (l.data() + skip, (int) l.size() - 3 * skip, 1, 352800.0, 1000.0)
                                                         : std::pair<double, double> { 0, 1 };
        t.check (rig.cap->config.wire == nx::WireFormat::Pcm && rig.cap->config.sampleRate == 352800.0,
                 "moteur : DSD64 -> DXD 352,8 kHz sur l'Anubis virtuelle, aucun DoP"_t);
        t.check (fit.first > 0.95 && peak <= 0.9773 && rig.engine->getStats().clipped == 0,
                 "+6 dB : amplitude "_t + juce::String (fit.first, 4) + ", crête " + juce::String (peak, 4) + " (limiteur −0,2 dBFS), aucun écrêtage"_t);
        t.check (profileNode, "Signal Path : profil Anubis affiché"_t);
    }

    // Pure DoP "Sync Safe": 1,5 s of DoP silence (0x69 + markers) before the first DSD byte, 2048-frame buffer
    {
        EngineRig rig ([&] (nx::CaptureBackend& c) { c.name = anubis; });
        rig.engine->setDevice (anubis);
        auto s = rig.engine->getSettings();
        s.anubisProfile = nx::AnubisProfile::PureDoP;
        rig.engine->setSettings (s);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd();
        const auto& got = rig.cap->capturedInt;
        size_t idleFrames = 0;
        bool markersOk = true;
        uint8_t last = 0;
        for (size_t i = 0; i + 1 < got.size(); i += 2)
        {
            const auto w = (uint32_t) got[i];
            const uint8_t marker = (uint8_t) (w >> 24);
            markersOk &= (marker == 0x05 || marker == 0xFA) && marker != last;
            last = marker;
            if (((w >> 16) & 0xFF) == 0x69 && ((w >> 8) & 0xFF) == 0x69) ++idleFrames; else break;
        }
        const size_t musicStart = idleFrames;
        bool payloadOk = got.size() / 2 >= musicStart + 1000;
        for (size_t i = 0; payloadOk && i < 1000; ++i)
        {
            const auto wl = (uint32_t) got[(musicStart + i) * 2];
            payloadOk &= (uint8_t) (wl >> 16) == dsd[i * 4] && (uint8_t) (wl >> 8) == dsd[i * 4 + 2];
        }
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->config.bufferFrames == 2048 && rig.cap->config.numChannels == 2,
                 "Pure DoP : DoP 176,4 kHz, tampon matériel 2048, 2 canaux"_t);
        t.check (idleFrames >= (size_t) (1.5 * 176400.0) && markersOk && payloadOk,
                 "silence DoP de "_t + juce::String (idleFrames / 176400.0, 2) + " s (marqueurs alternés) puis DSD intact"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (110, "Boucle A-B sans coupure : le DAC n'est jamais rouvert, fondu de 4 ms en PCM")
{
    bool stillRunning = false, loopWasActive = false;
    juce::uint64 reconfigAfter = 0;
    // PCM 44,1 kHz / 24 bits, 3 s of noise (every frame unique)
    const int rate = 44100;
    const auto pcm = randomPcm (rate * 3, 2, 24, 110);
    auto f = t.tempDir.getChildFile ("loop110.wav");
    writeWav (f, &pcm, nullptr, 2, (double) rate, 24, 24, false);

    EngineRig rig;
    rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
    // play 0,3 s, then loop [0,5 s, 0,8 s) for 2,5 s
    size_t mark = 0;
    juce::uint64 reconfigBefore = 0;
    double maxPos = 0.0, minPosAfterWrap = 1.0e9;
    rig.runToEnd (512, 60.0, [&] (int)
    {
        const size_t outFrames = rig.cap->capturedInt.size() / 2;
        if (mark == 0 && outFrames >= (size_t) (rate * 0.3))
        {
            mark = outFrames;
            reconfigBefore = rig.engine->getStats().reconfigurations.load();
            rig.engine->setLoop (0.5, 0.8);
            juce::Thread::sleep (250);   // the virtual DAC runs faster than real time: let the decoder refill first
        }
        else if (mark > 0 && outFrames > mark + (size_t) (rate * 0.5))
        {
            const double p = rig.engine->getPositionSeconds();
            maxPos = juce::jmax (maxPos, p);
            minPosAfterWrap = juce::jmin (minPosAfterWrap, p);
        }
        if (mark > 0 && outFrames >= mark + (size_t) (rate * 2.5) && rig.engine->getState() != nx::PlaybackEngine::State::Stopped)
        {
            stillRunning = rig.cap->isRunning();
            reconfigAfter = rig.engine->getStats().reconfigurations.load();
            loopWasActive = rig.engine->isLoopActive();
            rig.engine->stop();
        }
    });
    const auto& got = rig.cap->capturedInt;
    t.check (reconfigAfter == reconfigBefore && stillRunning && loopWasActive,
             "le périphérique n'est ni arrêté ni rouvert pendant la boucle"_t);

    // Find the loop in the output: frames of A + xf onwards repeat with a period of (B − A − xf) frames.
    const int a = (int) (0.5 * rate), b = (int) (0.8 * rate);
    const int xf = (int) (nx::PlaybackEngine::loopCrossfadeMs * 0.001 * rate);
    const int period = b - a - xf;
    const size_t frames = got.size() / 2;
    auto frameAt = [&] (size_t i, int c) { return got[i * 2 + (size_t) c]; };
    size_t first = 0;
    for (size_t i = mark; i + 64 < frames && first == 0; ++i)
    {
        bool same = true;
        for (int k = 0; k < 64 && same; ++k)
            same = frameAt (i + (size_t) k, 0) == pcm[(size_t) (a + xf + k) * 2] && frameAt (i + (size_t) k, 1) == pcm[(size_t) (a + xf + k) * 2 + 1];
        if (same) first = i;
    }
    int cycles = 0;
    bool exact = first > 0;
    for (size_t start = first; exact && start + (size_t) period < frames; start += (size_t) period, ++cycles)
        for (int k = 0; k < period - xf && exact; ++k)
            exact = frameAt (start + (size_t) k, 0) == pcm[(size_t) (a + xf + k) * 2] && frameAt (start + (size_t) k, 1) == pcm[(size_t) (a + xf + k) * 2 + 1];
    t.check (exact && cycles >= 4, juce::String (cycles) + " tours de boucle, période exacte de "_t + juce::String (period)
                                   + " trames, données bit-perfect hors des 4 ms de fondu"_t);

    // The crossfade: no jump larger than the signal's own range at the wrap (continuity of the mix)
    bool mixed = true;
    for (size_t start = first + (size_t) (period - xf); start + (size_t) xf < frames && mixed; start += (size_t) period)
    {
        const auto mid = (double) frameAt (start + (size_t) xf / 2, 0);
        const auto t0 = (double) pcm[(size_t) (b - xf + xf / 2) * 2], h0 = (double) pcm[(size_t) (a + xf / 2) * 2];
        mixed = std::abs (mid - 0.5 * (t0 + h0)) < 0.02 * 2147483648.0;
    }
    t.check (mixed, "fondu enchaîné fin -> début au milieu du raccord (moitié / moitié)"_t);
    t.check (maxPos <= 0.8 + 0.02 && minPosAfterWrap >= 0.49, "position affichée dans la boucle ("_t + juce::String (minPosAfterWrap, 2) + " – "
                                                               + juce::String (maxPos, 2) + " s)");

    rig.engine->clearLoop();
    t.check (! rig.engine->isLoopActive(), "boucle désactivée (touche L)"_t);

    // DSD in DoP: the loop cuts on DoP word boundaries, the markers keep alternating, the DAC stays locked.
    {
        const auto dsd = dsdSine (2822400.0, 1.5, 2, 1000.0, 0.5);
        auto fd = t.tempDir.getChildFile ("loop110.dsf");
        writeDsf (fd, dsd, 2, 2822400.0, true);
        EngineRig dr;
        dr.engine->setQueue ({ EngineRig::item (fd) }, 0, true);
        size_t dmark = 0;
        juce::uint64 before = 0, after = 0;
        dr.runToEnd (512, 60.0, [&] (int)
        {
            const size_t n = dr.cap->capturedInt.size() / 2;
            if (dmark == 0 && n >= 30000) { dmark = n; before = dr.engine->getStats().reconfigurations.load(); dr.engine->setLoop (0.3, 0.5); juce::Thread::sleep (250); }
            if (dmark > 0 && n >= dmark + 176400 && dr.engine->getState() != nx::PlaybackEngine::State::Stopped)
            {
                after = dr.engine->getStats().reconfigurations.load();
                dr.engine->stop();
            }
        });
        const auto& got = dr.cap->capturedInt;
        bool markers = true;
        uint8_t last = 0;
        for (size_t i = 0; i + 1 < got.size(); i += 2)
        {
            const uint8_t m = (uint8_t) ((uint32_t) got[i] >> 24);
            markers &= (m == 0x05 || m == 0xFA) && m != last;
            last = m;
        }
        t.check (dr.cap->config.wire == nx::WireFormat::DoP && markers && after == before && dmark > 0,
                 "DoP : boucle sur des mots DoP entiers, marqueurs 0x05 / 0xFA continus, DAC jamais reconfiguré"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (112, "Étape 6 : images SACD (.iso) : TOC, SACD Text, zones 2ch / 6ch, lecture DSD, DST")
{
    using namespace sacdfixture;
    const std::vector<AreaSpec> areas { { 2, { { 10, "Nocturne n°1", "Quatuor" }, { 7, "Nocturne n°2", "Quatuor" } } },
                                        { 6, { { 4, "Nocturne (multicanal)", "Quatuor" } } } };
    const auto bytes = build (areas, "Nocturnes SACD", "Quatuor \xC9" "b\xE8" "ne");   // Latin-1, as many discs store it
    const auto dir = t.tempDir.getChildFile ("sacd");
    dir.createDirectory();
    const auto iso = dir.getChildFile ("Nocturnes.iso");
    iso.replaceWithData (bytes.data(), bytes.size());

    nx::sacd::Disc disc;
    juce::String err;
    const bool ok = nx::sacd::readDisc (iso, disc, err);
    t.check (ok && disc.areas.size() == 2 && disc.albumTitle == "Nocturnes SACD" && disc.albumArtist == "Quatuor Ébène"_t && disc.year == 2004,
             "Master TOC (secteur 510) + SACD Text : album, artiste (Latin-1), année"_t + " " + err);
    if (! ok || disc.areas.size() != 2) return;
    const auto& st = disc.areas[0];
    const auto& mc = disc.areas[1];
    t.check (st.channels == 2 && mc.channels == 6 && st.tracks.size() == 2 && mc.tracks.size() == 1 && ! st.isDst()
             && st.tracks[0].title == "Nocturne n°1"_t && st.tracks[1].performer == "Quatuor" && st.tracks[0].frameCount == 10
             && std::abs (st.tracks[1].durationSeconds() - 7.0 / 75.0) < 1e-9,
             "Area TOC 2ch / 6ch : pistes (SACDTRL1 / 2), titres et interprètes (SACDTTxt), durées"_t);

    // every byte of a track, as DSF / DFF deliver them (MSB first, interleaved by byte)
    auto decodeAll = [] (nx::AudioDecoder& d)
    {
        std::vector<uint8_t> out, buf ((size_t) 4096 * (size_t) d.getFormat().numChannels);
        for (int n; (n = d.readDsd (buf.data(), 4096)) > 0;) out.insert (out.end(), buf.begin(), buf.begin() + n * d.getFormat().numChannels);
        return out;
    };
    {
        auto d = nx::openDecoder (juce::File (nx::sacd::virtualUri (iso, 0, 2)), err);
        const bool fmt = d != nullptr && d->getFormat().isDsd() && d->getFormat().sampleRate == 2822400.0 && d->getFormat().numChannels == 2
                      && d->getLength() == 7 * kFrameBytes && d->getFormat().codec == "SACD";
        t.check (fmt && decodeAll (*d) == expected (0, 2, 2, 7), "piste 2 (2ch) : DSD64 brut identique, en-têtes et paquets supplémentaires retirés"_t + " " + err);
    }
    {
        auto d = nx::openDecoder (juce::File (nx::sacd::virtualUri (iso, 1, 1)), err);
        t.check (d != nullptr && d->getFormat().numChannels == 6 && decodeAll (*d) == expected (1, 1, 6, 4), "zone multicanal : 6 canaux identiques"_t);
    }
    {
        // seek inside a frame of track 1, then read across frame and sector boundaries
        juce::FileInputStream raw (iso);
        auto d = nx::openDecoder (std::make_unique<juce::FileInputStream> (iso), nx::sacd::virtualUri (iso, 0, 1), err);
        const auto ref = expected (0, 1, 2, 10);
        bool same = d != nullptr;
        for (juce::int64 pos : { (juce::int64) 0, (juce::int64) 3 * kFrameBytes + 100, (juce::int64) 9 * kFrameBytes + 4000, (juce::int64) 5 * kFrameBytes })
        {
            if (! same) break;
            same = d->seek (pos) && d->getPosition() == pos;
            std::vector<uint8_t> buf (2 * 1000);
            const int n = d->readDsd (buf.data(), 1000);
            const auto expectN = (int) std::min<juce::int64> (1000, 10 * kFrameBytes - pos);
            same = same && n == expectN && std::equal (buf.begin(), buf.begin() + n * 2, ref.begin() + pos * 2);
        }
        t.check (same, "recherche (seek) au milieu d'une trame : octets exacts"_t);
    }
    {
        // not a SACD image / bad paths
        const auto junk = dir.getChildFile ("data.iso");
        junk.replaceWithData (std::vector<uint8_t> (600 * kSector, 0).data(), 600 * kSector);
        nx::sacd::Disc dd;
        juce::File iso2;
        int a = 0, tr = 0;
        t.check (! nx::sacd::readDisc (junk, dd, err) && ! nx::sacd::parseVirtualUri ("/x/a.iso#area=3&track=1", iso2, a, tr)
                 && nx::sacd::parseVirtualUri (nx::sacd::virtualUri (iso, 1, 12), iso2, a, tr) && iso2 == iso && a == 1 && tr == 12
                 && nx::sacd::realFile (nx::sacd::virtualUri (iso, 0, 1)) == iso,
                 "ISO sans SACDMTOC ignorée, chemins virtuels « album.iso#area=0&track=1 »"_t);
    }

    // through the engine, exactly like a .dsf: native DSD bytes, then DoP
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; });
        auto item = EngineRig::item (iso);
        item.uri = nx::sacd::virtualUri (iso, 0, 2);
        rig.engine->setQueue ({ item }, 0, true);
        rig.runToEnd (512);
        const auto ref = expected (0, 2, 2, 7);
        const auto& got = rig.cap->capturedDsd;
        t.check (rig.cap->config.wire == nx::WireFormat::NativeDsd && got.size() >= ref.size() && std::equal (ref.begin(), ref.end(), got.begin()),
                 "moteur : piste d'ISO en DSD natif, octets identiques ("_t + juce::String ((int) got.size()) + ")");
    }
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 44100.0, 88200.0, 176400.0 }; });
        auto item = EngineRig::item (iso);
        item.uri = nx::sacd::virtualUri (iso, 0, 1);
        rig.engine->setQueue ({ item }, 0, true);
        rig.runToEnd (512);
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->capturedInt.size() > 1000, "moteur : piste d'ISO en DoP 176,4 kHz"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (113, "Ordre de lecture : répétition (piste / file) et lecture aléatoire sans répétition")
{
    using R = nx::PlaybackEngine::Repeat;
    // queue logic alone (no audio): next index in play order
    {
        EngineRig rig;
        std::vector<nx::PlayableItem> items (5);
        rig.engine->setQueue (items, 0, false);
        auto& e = *rig.engine;
        t.check (e.getNextIndex (1) == 2 && e.getNextIndex (4) == -1, "ordre normal : suite de la file, fin après la dernière"_t);
        e.setRepeat (R::All);
        t.check (e.getNextIndex (4) == 0 && e.getNextIndex (2) == 3, "Répéter la file : la dernière piste est suivie de la première"_t);
        e.setRepeat (R::One);
        t.check (e.getNextIndex (2) == 2 && e.getNextIndex (2, true) == 3 && e.getNextIndex (4, true) == 0,
                 "Répéter la piste : la même piste en fin naturelle, Suivant passe quand même à la suivante"_t);
        e.setRepeat (R::Off);
        e.setShuffle (true);
        std::set<int> seen { 0 };
        int cur = 0;
        for (int k = 0; k < 4; ++k) { cur = e.getNextIndex (cur); seen.insert (cur); }
        t.check (seen.size() == 5 && e.getNextIndex (cur) == -1 && e.getRemainingCount() == 4,
                 "aléatoire : chaque piste exactement une fois, puis fin de la file"_t);
        e.setRepeat (R::All);
        const int last = cur;
        std::set<int> pass2;
        for (int k = 0; k < 4; ++k) { cur = e.getNextIndex (cur); pass2.insert (cur); }
        t.check (pass2.size() == 4 && pass2.count (last) == 0,
                 "aléatoire + répétition : nouvelle passe sans doublon, la dernière piste n'est pas rejouée aussitôt"_t);
        e.setShuffle (false);
        t.check (! e.isShuffle() && e.getNextIndex (1) == 2, "aléatoire coupé : retour à l'ordre de la file"_t);
    }

    // through the audio path: gapless repeat of the track / of the queue, shuffled pass
    const auto a = randomPcm (6000, 2, 16, 11), b = randomPcm (5000, 2, 16, 12), c = randomPcm (4000, 2, 16, 13), d = randomPcm (3000, 2, 16, 14);
    juce::Array<juce::File> files;
    int n = 0;
    for (auto* pcm : { &a, &b, &c, &d })
    {
        auto f = t.tempDir.getChildFile ("ord" + juce::String (n++) + ".wav");
        writeWav (f, pcm, nullptr, 2, 48000, 16, 16, false);
        files.add (f);
    }
    auto itemsOf = [&] (std::initializer_list<int> ids) { std::vector<nx::PlayableItem> v; for (int i : ids) v.push_back (EngineRig::item (files[i])); return v; };
    auto startsWith = [] (const std::vector<int32_t>& got, size_t at, const std::vector<int32_t>& ref)
    {
        return got.size() >= at + ref.size() && std::equal (ref.begin(), ref.end(), got.begin() + (std::ptrdiff_t) at);
    };
    {
        EngineRig rig;
        int changes = 0;
        rig.engine->onTrackChanged = [&] { ++changes; };
        rig.engine->setRepeat (R::One);
        rig.engine->setQueue (itemsOf ({ 0, 1 }), 0, true);
        rig.runToEnd (1024, 30.0, [&] (int) { if (changes >= 3) rig.engine->setRepeat (R::Off); });
        const auto& got = rig.cap->capturedInt;
        t.check (startsWith (got, 0, a) && startsWith (got, a.size(), a) && startsWith (got, 2 * a.size(), a),
                 "Répéter la piste : la piste 1 repart à 0:00 sans coupure quand elle se termine"_t);
    }
    {
        EngineRig rig;
        int changes = 0;
        rig.engine->onTrackChanged = [&] { ++changes; };
        rig.engine->setRepeat (R::All);
        rig.engine->setQueue (itemsOf ({ 0, 1 }), 0, true);
        rig.runToEnd (1024, 30.0, [&] (int) { if (changes >= 4) rig.engine->setRepeat (R::Off); });
        const auto& got = rig.cap->capturedInt;
        t.check (startsWith (got, 0, a) && startsWith (got, a.size(), b) && startsWith (got, a.size() + b.size(), a)
                 && startsWith (got, 2 * a.size() + b.size(), b),
                 "Répéter la file : après la dernière piste, retour au début de la file (A B A B)"_t);
    }
    {
        EngineRig rig;
        std::vector<int> order;
        rig.engine->onTrackChanged = [&] { order.push_back (rig.engine->getCurrentIndex()); };
        rig.engine->setShuffle (true);
        rig.engine->setQueue (itemsOf ({ 0, 1, 2, 3 }), 0, true);
        rig.runToEnd (1024, 30.0);
        std::set<int> distinct (order.begin(), order.end());
        t.check (order.size() == 4 && distinct.size() == 4 && order.front() == 0,
                 "aléatoire : la lecture réelle passe une fois par chaque piste, celle choisie en premier"_t);
        // the audio follows the same order
        const auto& got = rig.cap->capturedInt;
        size_t at = 0;
        bool exact = ! order.empty();
        for (int idx : order)
        {
            const auto& ref = idx == 0 ? a : idx == 1 ? b : idx == 2 ? c : d;
            exact = exact && startsWith (got, at, ref);
            at += ref.size();
        }
        t.check (exact, "aléatoire : les échantillons joués suivent l'ordre mélangé, sans trou"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (114, "Partie 2/3 : décodeur DST (MPEG-4), SACD / DFF compressés, 6 canaux sur une sortie stéréo")
{
    auto fnv = [] (const uint8_t* p, size_t n) { unsigned long long h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; } return h; };
    const int fb = dsttest::frameBytesPerChannel;

    // 1. reference frames (checked with the MPEG-4 reference decoder and FFmpeg)
    for (auto [b64, ch, hash, name] : { std::make_tuple (dstgolden::stereoFrame, dstgolden::stereoChannels, dstgolden::stereoDsdHash, "2 canaux, Rice, une table par canal"),
                                        std::make_tuple (dstgolden::multichannelFrame, dstgolden::multichannelChannels, dstgolden::multichannelDsdHash, "6 canaux, 2 segments") })
    {
        juce::MemoryOutputStream mo;
        juce::Base64::convertFromBase64 (mo, b64);
        nx::dst::FrameDecoder d (ch);
        std::vector<uint8_t> out ((size_t) d.getOutputSize());
        juce::String err;
        const bool ok = d.decode (static_cast<const uint8_t*> (mo.getData()), mo.getDataSize(), out.data(), err);
        t.check (ok && fnv (out.data(), out.size()) == hash, "trame DST de référence ("_t + juce::String (name) + ") : DSD64 identique au bit près"_t + " " + err);
    }

    // 2. frames from the encoder: every coding option, lossless
    const auto dsd2 = dsttest::sigmaDelta (2, 3, 0.5, 9), dsd6 = dsttest::sigmaDelta (6, 2, 0.5, 9);
    {
        struct Case { const char* name; int ch; dsttest::Options o; };
        std::vector<Case> cases;
        cases.push_back ({ "par défaut", 2, {} });
        { dsttest::Options o; o.perChannelTables = false; o.halfProbOddChannels = false; cases.push_back ({ "tables communes", 2, o }); }
        { dsttest::Options o; o.allowRice = false; o.filterOrder = 128; o.ptableLength = 64; cases.push_back ({ "filtre 128 coefficients, tables non codées", 2, o }); }
        { dsttest::Options o; o.twoSegments = true; cases.push_back ({ "2 segments", 2, o }); }
        { dsttest::Options o; o.filterOrder = 13; o.ptableLength = 7; cases.push_back ({ "6 canaux", 6, o }); }
        { dsttest::Options o; o.twoSegments = true; o.perChannelTables = false; cases.push_back ({ "6 canaux, 2 segments", 6, o }); }
        bool allOk = true;
        size_t coded = 0, plainSize = 0;
        juce::String failed;
        for (auto& c : cases)
        {
            const auto& src = c.ch == 2 ? dsd2 : dsd6;
            nx::dst::FrameDecoder d (c.ch);
            std::vector<uint8_t> out ((size_t) d.getOutputSize());
            const auto frame = dsttest::encodeFrame (src.data(), c.ch, c.o);
            juce::String err;
            const bool ok = d.decode (frame.data(), frame.size(), out.data(), err) && std::equal (out.begin(), out.end(), src.begin());
            if (! ok) failed << juce::String::fromUTF8 (c.name) << " (" << err << ") ";
            allOk = allOk && ok;
            coded += frame.size();
            plainSize += out.size();
        }
        t.check (allOk, "trames DST codées : segmentation, correspondances, Ptables, coefficients Rice, demi-probabilité, sans perte "_t + failed);
        t.check (coded < plainSize * 6 / 10, "compression réelle : "_t + juce::String ((double) coded / (double) plainSize, 2) + " de la taille DSD");

        nx::dst::FrameDecoder d (2);
        std::vector<uint8_t> out ((size_t) d.getOutputSize());
        const auto plain = dsttest::plainFrame (dsd2.data(), 2);
        juce::String err;
        t.check (d.decode (plain.data(), plain.size(), out.data(), err) && d.lastFrameWasPlain() && std::equal (out.begin(), out.end(), dsd2.begin()),
                 "trame non compressée (DST_Coded = 0) : DSD recopié tel quel"_t);

        // corrupt / truncated frames: an error and DSD silence, never a crash
        auto bad = dsttest::plainFrame (dsd2.data(), 2);
        bad[0] = 0x05;                                  // stuffing bits must be 0
        const bool e1 = ! d.decode (bad.data(), bad.size(), out.data(), err) && out[100] == 0x69;
        auto cut = dsttest::encodeFrame (dsd2.data(), 2);
        cut.resize (12);                                // the tables are cut off
        const bool e2 = ! d.decode (cut.data(), cut.size(), out.data(), err);
        const bool e3 = ! d.decode (nullptr, 0, out.data(), err);
        t.check (e1 && e2 && e3, "trame corrompue ou tronquée : erreur signalée, silence DSD, pas de plantage"_t);

        // real time: a 6-channel frame (1/75 s) decodes much faster than it plays
        const auto f6 = dsttest::encodeFrame (dsd6.data(), 6);
        nx::dst::FrameDecoder d6 (6);
        std::vector<uint8_t> o6 ((size_t) d6.getOutputSize());
        double best = 1e9;
        for (int k = 0; k < 3; ++k)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            d6.decode (f6.data(), f6.size(), o6.data(), err);
            best = juce::jmin (best, juce::Time::getMillisecondCounterHiRes() - t0);
        }
        t.check (best < 1000.0 / 75.0, "temps réel : une trame 6 canaux (13,3 ms de musique) décodée en "_t + juce::String (best, 2) + " ms");
    }

    // 3. DSDIFF files compressed in DST (with and without the DSTI index)
    {
        std::vector<std::vector<uint8_t>> frames;
        for (int k = 0; k < 3; ++k) frames.push_back (k == 1 ? dsttest::plainFrame (dsd2.data() + (size_t) k * fb * 2, 2)
                                                             : dsttest::encodeFrame (dsd2.data() + (size_t) k * fb * 2, 2));
        bool ok = true;
        for (bool index : { true, false })
        {
            auto f = t.tempDir.getChildFile (index ? "dst_index.dff" : "dst_walk.dff");
            writeDffDst (f, frames, 2, index);
            nx::StreamFormat fmt;
            const auto back = decodeAllDsd (f, &fmt);
            ok = ok && back == dsd2 && fmt.codec == "DFF DST";
            juce::String err;
            auto d = nx::openDecoder (f, err);
            std::vector<uint8_t> buf (2 * 3000);
            ok = ok && d != nullptr && d->getLength() == 3 * fb && d->seek (2 * fb - 1000) && d->readDsd (buf.data(), 3000) == 3000
                 && std::equal (buf.begin(), buf.end(), dsd2.begin() + (2 * fb - 1000) * 2);
        }
        t.check (ok, "DFF compressé en DST : décodé à l'octet près, recherche à cheval sur deux trames (avec / sans index DSTI)"_t);

        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; });
        rig.engine->setQueue ({ EngineRig::item (t.tempDir.getChildFile ("dst_index.dff")) }, 0, true);
        rig.runToEnd (512);
        const auto& got = rig.cap->capturedDsd;
        t.check (rig.cap->config.wire == nx::WireFormat::NativeDsd && got.size() >= dsd2.size() && std::equal (dsd2.begin(), dsd2.end(), got.begin()),
                 "moteur : DFF DST en DSD natif, flux identique au DSD d'origine"_t);
    }

    // 4. SACD images with DST areas (frames across sectors, 4-byte frame info)
    using namespace sacdfixture;
    std::vector<AreaSpec> areas { { 2, { { 6, "Allegro", "Quatuor" }, { 4, "Adagio", "Quatuor" } } },
                                  { 6, { { 3, "Allegro (5.1)", "Quatuor" } } } };
    areas[0].frameFormat = 0;
    areas[1].frameFormat = 0;
    const auto img = build (areas, "Quatuors DST", "Quatuor");
    const auto iso = t.tempDir.getChildFile ("sacd_dst").getChildFile ("Quatuors.iso");
    iso.getParentDirectory().createDirectory();
    iso.replaceWithData (img.data(), img.size());
    {
        nx::sacd::Disc disc;
        juce::String err;
        const bool toc = nx::sacd::readDisc (iso, disc, err) && disc.areas.size() == 2 && disc.areas[0].isDst() && disc.areas[1].isDst();
        auto readAll = [] (nx::AudioDecoder& d)
        {
            std::vector<uint8_t> out, buf ((size_t) 4096 * (size_t) d.getFormat().numChannels);
            for (int n; (n = d.readDsd (buf.data(), 4096)) > 0;) out.insert (out.end(), buf.begin(), buf.begin() + n * d.getFormat().numChannels);
            return out;
        };
        auto a = nx::openDecoder (juce::File (nx::sacd::virtualUri (iso, 0, 2)), err);
        const bool st = a != nullptr && a->getFormat().codec == "SACD DST" && readAll (*a) == expected (0, 2, 2, 4);
        auto m = nx::openDecoder (juce::File (nx::sacd::virtualUri (iso, 1, 1)), err);
        const bool mc = m != nullptr && m->getFormat().numChannels == 6 && readAll (*m) == expected (1, 1, 6, 3);
        t.check (toc && st && mc, "ISO SACD en DST : zones 2ch et 6ch décompressées à la volée, DSD64 identique "_t + err
                 + " [" + juce::String ((int) toc) + juce::String ((int) st) + juce::String ((int) mc) + "]");

        auto s = nx::openDecoder (juce::File (nx::sacd::virtualUri (iso, 0, 1)), err);
        const auto ref = expected (0, 1, 2, 6);
        bool same = s != nullptr;
        for (juce::int64 pos : { (juce::int64) 4 * kFrameBytes + 77, (juce::int64) 0, (juce::int64) 2 * kFrameBytes + 4700 })
        {
            std::vector<uint8_t> buf (2 * 800);
            same = same && s->seek (pos) && s->readDsd (buf.data(), 800) == 800 && std::equal (buf.begin(), buf.end(), ref.begin() + pos * 2);
        }
        t.check (same, "ISO DST : recherche au milieu d'une trame compressée, octets exacts"_t);

        // track list whose start times disagree with the sectors' time codes (seen on some rips): still plays from its first frame
        bool tolerant = true;
        for (int shift : { 60 * 75, -7 })
        {
            auto bad = img;
            auto* t2 = bad.data() + (size_t) 542 * kSector + 8;           // SACDTRL2 of the 2ch area, track 1 start time
            Image::time (t2, juce::jmax (0, 150 + shift));
            const auto f = t.tempDir.getChildFile ("sacd_dst").getChildFile ("Decale" + juce::String (shift) + ".iso");
            f.replaceWithData (bad.data(), bad.size());
            auto d = nx::openDecoder (juce::File (nx::sacd::virtualUri (f, 0, 1)), err);
            tolerant = tolerant && d != nullptr && readAll (*d) == expected (0, 1, 2, 6);
        }
        t.check (tolerant, "pistes 2ch dont la liste des pistes ne correspond pas aux codes temporels : lues depuis leur première trame"_t);
    }

    // 5. a 6-channel track on a DAC with 2 outputs: never refused, and the disc's own stereo area plays (Sous-étape F)
    auto mcItem = EngineRig::item (iso);
    mcItem.uri = nx::sacd::virtualUri (iso, 1, 1);
    const auto stereoRef = expected (0, 1, 2, 6);
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; c.capabilities.outputChannels = 2; });
        nx::SignalPath path;
        rig.engine->setQueue ({ mcItem }, 0, true);
        rig.runToEnd (512, 60.0, [&] (int block) { if (block == 5) path = rig.engine->getSignalPath(); });
        const auto& got = rig.cap->capturedDsd;
        bool layer = false;
        for (auto& n : path.nodes) layer = layer || n.title.contains ("Couche");
        t.check (rig.cap->config.numChannels == 2 && rig.cap->config.wire == nx::WireFormat::NativeDsd && got.size() >= stereoRef.size()
                 && std::equal (stereoRef.begin(), stereoRef.end(), got.begin()) && layer,
                 "SACD 6 canaux sur un DAC stéréo : la couche stéréo du disque est lue au bit près (DSD natif), affichée dans le chemin du signal"_t);
    }
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 44100.0, 88200.0, 176400.0 }; c.capabilities.outputChannels = 2; });
        rig.engine->setQueue ({ mcItem }, 0, true);
        rig.runToEnd (512);
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->config.numChannels == 2 && rig.cap->capturedInt.size() > 1000,
                 "6 canaux DSD sur un DAC stéréo (DoP) : flux stéréo valide"_t);
    }
    {
        // a multichannel DSF has no stereo layer: ITU mix computed in DXD, never front L / R alone
        const auto six = dsdSine (2822400.0, 0.2, 6, 1000.0, 0.5);
        auto f = t.tempDir.getChildFile ("six.dsf");
        writeDsf (f, six, 6, 2822400.0, true);
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 88200.0, 176400.0, 352800.0 }; c.capabilities.outputChannels = 2; });
        auto st = rig.engine->getSettings();
        st.keepDsdOutput = false;                       // DXD output (the DSD one is checked in section 119)
        rig.engine->setSettings (st);
        nx::SignalPath path;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (512, 60.0, [&] (int block) { if (block == 5) path = rig.engine->getSignalPath(); });
        bool stage = false;
        for (auto& n : path.nodes) stage = stage || n.title.contains ("ITU");
        t.check (rig.cap->config.wire == nx::WireFormat::Pcm && rig.cap->config.numChannels == 2 && rig.cap->config.sampleRate == 352800.0
                 && rig.engine->getCurrentPlan().downmix && stage && rig.cap->capturedInt.size() > 1000,
                 "DSF 6 canaux sur un DAC stéréo (même compatible DoP) : mixage ITU 5.1 -> 2.0 en DXD 352,8 kHz, étape dans le chemin du signal"_t);
    }

    // PCM 5.1: the 2.0 mix follows L = FL + 0,707 C + 0,707 SL (normalised), never clips; 6 outputs: 6 channels as they are
    {
        const int frames = 24000;
        std::vector<int32_t> pcm ((size_t) frames * 6);
        std::vector<double> src ((size_t) frames * 6);
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < 6; ++c)
            {
                // FL, C and SL in phase at 0.99 of full scale: the worst case for the left mix
                const double amp = (c == 0 || c == 2 || c == 4) ? 0.99 : 0.3 + 0.05 * c;
                const double v = amp * std::sin (2.0 * juce::MathConstants<double>::pi * (c == 0 || c == 2 || c == 4 ? 500.0 : 700.0 + 50 * c) * i / 48000.0);
                const auto q = (int32_t) std::lround (v * 8388607.0) * 256;
                pcm[(size_t) i * 6 + (size_t) c] = q;
                src[(size_t) i * 6 + (size_t) c] = q / 2147483648.0;
            }
        auto f = t.tempDir.getChildFile ("surround51.wav");
        writeWav (f, &pcm, nullptr, 6, 48000, 24, 24, false);

        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.outputChannels = 2; });
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (1024);
        const auto& got = rig.cap->capturedInt;
        const double k = 0.70710678118654752, g = 1.0 / (1.0 + 2.0 * k);
        double maxErr = 0.0, peak = 0.0;
        bool ok = rig.cap->config.numChannels == 2 && got.size() >= (size_t) frames * 2;
        for (int i = 0; ok && i < frames; ++i)
        {
            const double* s6 = src.data() + (size_t) i * 6;
            const double l = g * (s6[0] + k * s6[2] + k * s6[4]), r = g * (s6[1] + k * s6[2] + k * s6[5]);
            maxErr = juce::jmax (maxErr, std::abs (got[(size_t) i * 2] / 2147483648.0 - l), std::abs (got[(size_t) i * 2 + 1] / 2147483648.0 - r));
            peak = juce::jmax (peak, std::abs (got[(size_t) i * 2] / 2147483648.0));
        }
        t.check (ok && maxErr < 4.0 / 8388608.0 && peak < 1.0 && peak > 0.97,
                 "PCM 5.1 sur une sortie stéréo : mixage exact à 4 LSB près (24 bits), crête "_t + juce::String (peak, 4) + ", sans écrêtage"_t);

        EngineRig six ([] (nx::CaptureBackend& c) { c.capabilities.outputChannels = 8; });
        six.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        six.runToEnd (1024);
        const auto& g6 = six.cap->capturedInt;
        t.check (six.cap->config.numChannels == 6 && ! six.engine->getCurrentPlan().downmix && g6.size() >= pcm.size()
                 && std::equal (pcm.begin(), pcm.end(), g6.begin()),
                 "PCM 5.1 sur une interface 6+ sorties : 6 canaux directs, bit-perfect"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (115, "Édition Free dans le moteur : PCM jusqu'à 192 kHz, Studio Master refusés")
{
    auto play = [&] (const juce::File& f, juce::String& error, nx::CaptureBackend*& capOut, std::function<void (nx::CaptureBackend&)> cfg = {})
    {
        auto rig = std::make_unique<EngineRig> (cfg);
        auto s = rig->engine->getSettings();
        s.freeEdition = true;
        rig->engine->setSettings (s);
        rig->engine->onError = [&error] (const juce::String& m) { error = m; };
        rig->engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig->runToEnd (1024, 20.0);
        capOut = rig->cap;
        return rig;
    };
    const auto pcm192 = sinePcm (19200, 2, 24, 192000, 1000, 0.5);
    const auto pcm352 = sinePcm (35280, 2, 24, 352800, 1000, 0.5);
    auto f192 = t.tempDir.getChildFile ("free192.wav"), f352 = t.tempDir.getChildFile ("free352.wav"), fdsd = t.tempDir.getChildFile ("free.dsf");
    writeWav (f192, &pcm192, nullptr, 2, 192000, 24, 24, false);
    writeWav (f352, &pcm352, nullptr, 2, 352800, 24, 24, false);
    writeDsf (fdsd, dsdSine (2822400.0, 0.1, 2, 1000.0, 0.5), 2, 2822400.0, false);

    juce::String e1, e2, e3;
    nx::CaptureBackend *c1 = nullptr, *c2 = nullptr, *c3 = nullptr;
    auto r1 = play (f192, e1, c1);
    auto r2 = play (f352, e2, c2);
    auto r3 = play (fdsd, e3, c3, [] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; });
    t.check (e1.isEmpty() && c1->capturedInt.size() >= pcm192.size() && std::equal (pcm192.begin(), pcm192.end(), c1->capturedInt.begin()),
             "Free : PCM 24 bits / 192 kHz lu en bit-perfect"_t);
    t.check (e2.startsWith (nx::PlaybackEngine::studioMasterError) && c2->capturedInt.empty()
             && e3.startsWith (nx::PlaybackEngine::studioMasterError) && c3->capturedDsd.empty(),
             "Free : DXD 352,8 kHz et DSD refusés par le moteur (message « Premium »), rien n'atteint le DAC"_t);
    nx::EngineSettings s;
    s.freeEdition = true;
    s.exclusive = true;
    s.hardwareProfile = nx::HardwareProfile::MergingRavenna;
    t.check (! nx::resolveProfile (s, "Anubis").exclusive && nx::activeFamily (s, "Anubis") == nx::DeviceFamily::None
             && nx::resolveProfile (s, "Anubis").profileTag.isEmpty(),
             "Free : mode exclusif et profils matériels neutralisés (profil standard)"_t);
}

//==============================================================================
NX_TEST_SECTION (116, "Sous-étape A : DAC qui ne cadence plus (reprise complète, sortie partagée, message clair)")
{
    // Logs of 16 and 21 Nov 2026: the device opens and starts, then asks for no buffer (Anubis after a switch to DoP,
    // FiiO KA17 at 384 kHz). The engine must climb its recovery ladder instead of looping on the same dead stream.
    const auto pcm = randomPcm (96000, 2, 24, 4242u);
    auto wav = t.tempDir.getChildFile ("stall.wav");
    writeWav (wav, &pcm, nullptr, 2, 96000, 24, 24, false);
    auto dsf = t.tempDir.getChildFile ("stall.dsf");
    writeDsf (dsf, dsdSine (2822400.0, 0.4, 2, 1000.0, 0.5), 2, 2822400.0, true);

    struct Result { int opens = 0; bool shared = false, playing = false, finished = false, gotAudio = false, closed = false; juce::String error; };
    // clocks(exclusive) says whether the virtual DAC delivers callbacks in that mode
    auto run = [&] (const juce::File& f, std::function<bool (bool exclusive)> clocks)
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.realtime = true; });
        rig.engine->startPatienceMs = 150;
        Result r;
        rig.engine->onError = [&r] (const juce::String& m) { r.error = m; };
        rig.engine->onQueueFinished = [&r] { r.finished = true; };
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        const auto t0 = juce::Time::getMillisecondCounter();
        while (juce::Time::getMillisecondCounter() - t0 < 4000)
        {
            rig.engine->serviceMessageThread();
            if (rig.cap->isRunning() && clocks (rig.cap->config.exclusive))
            {
                for (int w = 0; w < 500 && rig.engine->isDecoderBusy() && rig.engine->getBufferedBytes() < (1u << 18); ++w) juce::Thread::sleep (1);
                rig.cap->pump (512);
            }
            if (rig.engine->getState() == nx::PlaybackEngine::State::Stopped && (r.error.isNotEmpty() || r.finished)) break;
            juce::Thread::sleep (2);
        }
        r.opens = rig.cap->opens;
        r.shared = rig.engine->isOnSharedFallback();
        r.playing = rig.engine->getState() == nx::PlaybackEngine::State::Playing;
        r.gotAudio = ! rig.cap->capturedInt.empty();
        r.closed = ! rig.cap->getStatus().open;
        return r;
    };

    // 1. exclusive access never clocks, the shared output does: playback goes on, and the user is told why
    const auto a = run (wav, [] (bool exclusive) { return ! exclusive; });
    t.check (a.opens == 3 && a.shared && (a.playing || a.finished) && a.gotAudio && a.error.contains ("partag"),
             "PCM : redémarrage, fermeture / réouverture, puis sortie système partagée : la lecture continue (ouvertures "_t
             + juce::String (a.opens) + ")");

    // 2. nothing clocks at all: stop, let go of the DAC, and say what to do
    const auto b = run (wav, [] (bool) { return false; });
    t.check (b.opens == 3 && ! b.playing && b.closed && b.error.contains ("Débranchez"),
             "PCM : le DAC ne répond toujours pas : arrêt, DAC libéré, message « débranchez / rebranchez »"_t);

    // 3. DoP: never the shared mixer (it would turn DoP into noise); full reopen, then the clear message
    const auto c = run (dsf, [] (bool) { return false; });
    t.check (c.opens == 2 && ! c.shared && ! c.playing && c.closed && c.error.contains ("Débranchez"),
             "DoP : fermeture / réouverture complète puis message clair, jamais de mixeur système (ouvertures "_t
             + juce::String (c.opens) + ")");

    // 4. a device that clocks normally is left alone (no reopen)
    const auto d = run (wav, [] (bool) { return true; });
    t.check (d.opens == 1 && ! d.shared && (d.playing || d.finished) && d.gotAudio && d.error.isEmpty(), "DAC normal : aucune reprise, une seule ouverture"_t);

    // 5. during playback (after the start watch), a DAC that stops asking for buffers for 600 ms is written in audio.log
    {
        // 4 s of music: whatever the speed of the machine, the track is still playing when the stall comes
        const auto longPcm = randomPcm (96000 * 4, 2, 24, 4243u);
        auto longWav = t.tempDir.getChildFile ("stall_long.wav");
        writeWav (longWav, &longPcm, nullptr, 2, 96000, 24, 24, false);
        // normal start patience: a slow machine (macOS CI) may take > 150 ms to deliver its first buffers here
        EngineRig rig ([] (nx::CaptureBackend& cap) { cap.realtime = true; });
        rig.engine->setQueue ({ EngineRig::item (longWav) }, 0, true);
        // a unique mark: the log may be trimmed meanwhile (512 KB cap), an offset would not survive that
        const auto mark = "test: repere blocage " + juce::String (juce::Random::getSystemRandom().nextInt64());
        nx::audioLog (mark);
        const auto t0 = juce::Time::getMillisecondCounter();
        bool paused = false;
        while (juce::Time::getMillisecondCounter() - t0 < 8000)
        {
            rig.engine->serviceMessageThread();
            const auto ms = juce::Time::getMillisecondCounter() - t0;
            const bool stalled = ms > 6000 && ms < 6600;            // after the 5 s start log, before the end of the track
            paused = paused || stalled;
            if (rig.cap->isRunning() && ! stalled)
            {
                for (int w = 0; w < 500 && rig.engine->isDecoderBusy() && rig.engine->getBufferedBytes() < (1u << 18); ++w) juce::Thread::sleep (1);
                rig.cap->pump (48);                                 // slow pump: the 1 s track lasts the whole test
            }
            juce::Thread::sleep (5);
        }
        const auto added = nx::audioLogFile().loadFileAsString().fromLastOccurrenceOf (mark, false, false);
        t.check (paused && rig.engine->getState() == nx::PlaybackEngine::State::Playing && rig.cap->opens == 1
                 && added.contains ("blocage du DAC, aucun buffer demande pendant"),
                 "audio.log : un blocage du DAC en cours de lecture est noté avec sa durée, la position et le ring"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (117, "Sous-étape F : multicanal Auto / Stéréo / Multicanal, couche stéréo SACD, correspondance des sorties")
{
    // 1. the plan's rules (pure function)
    nx::StreamFormat pcm6;
    pcm6.kind = nx::SampleKind::PcmInt; pcm6.sampleRate = 48000.0; pcm6.numChannels = 6; pcm6.bitsPerSample = 24;
    nx::StreamFormat dsd6 = pcm6;
    dsd6.kind = nx::SampleKind::Dsd; dsd6.sampleRate = 2822400.0; dsd6.bitsPerSample = 1;
    nx::DeviceCapabilities eight;
    eight.sampleRates = { 48000.0, 176400.0, 352800.0 };
    eight.outputChannels = 8;
    nx::EngineSettings s;
    s.maxChannels = 2;                                                       // a 2-channel device profile (Anubis Pure DoP)
    const auto autoPlan = nx::makePlan (pcm6, eight, s);
    s.channelMode = nx::ChannelMode::Multichannel;
    const auto multiPlan = nx::makePlan (pcm6, eight, s);
    s.maxChannels = 8;
    s.channelMode = nx::ChannelMode::Stereo;
    s.keepDsdOutput = false;                            // the mix to DXD PCM (to DSD: section 119)
    const auto stereoPlan = nx::makePlan (pcm6, eight, s);
    const auto stereoDsd = nx::makePlan (dsd6, eight, s);
    s.channelMode = nx::ChannelMode::Auto;
    const auto autoDsd = nx::makePlan (dsd6, eight, s);
    t.check (autoPlan.downmix && autoPlan.channels == 2 && ! multiPlan.downmix && multiPlan.channels == 6 && multiPlan.identityRoute()
             && stereoPlan.downmix && stereoPlan.channels == 2 && stereoDsd.downmix && stereoDsd.wire == nx::WireFormat::Pcm
             && stereoDsd.deviceRate == 352800.0 && ! autoDsd.downmix && autoDsd.wire == nx::WireFormat::DoP && autoDsd.channels == 6,
             "plan : Auto suit le profil (2 canaux -> mixage), Multicanal passe outre, Stéréo force le mixage (DSD via DXD 352,8 kHz)"_t);

    s.channelMap = { 0, 1, 7, 3, 4, 5, 6, 2 };                               // centre on output 8
    const auto mapped = nx::makePlan (pcm6, eight, s);
    s.channelMap = { 0, 1, 0, 3, 4, 5, 6, 7 };                               // two speakers on one output: refused
    const auto clash = nx::makePlan (pcm6, eight, s);
    nx::DeviceCapabilities sixOut = eight;
    sixOut.outputChannels = 6;
    s.channelMap = { 0, 1, 7, 3, 4, 5, 6, 2 };
    const auto tooFar = nx::makePlan (pcm6, sixOut, s);                      // output 8 does not exist on this device
    t.check (mapped.channels == 8 && mapped.route[7] == 2 && mapped.route[2] == -1 && mapped.route[0] == 0 && mapped.route[5] == 5 && ! mapped.identityRoute()
             && clash.identityRoute() && clash.channels == 6 && tooFar.identityRoute() && tooFar.channels == 6,
             "correspondance : centre vers la sortie 8 (sortie 3 muette) ; doublon ou sortie absente -> ordre standard"_t);

    // 2. PCM 5.1 through the engine with the centre on output 8: every sample where it belongs, output 3 silent
    {
        const int frames = 6000;
        std::vector<int32_t> pcm ((size_t) frames * 6);
        juce::Random rnd (617);
        for (auto& v : pcm) v = (rnd.nextInt (1 << 24) - (1 << 23)) * 256;
        auto f = t.tempDir.getChildFile ("map51.wav");
        writeWav (f, &pcm, nullptr, 6, 48000, 24, 24, false);
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.outputChannels = 8; });
        auto st = rig.engine->getSettings();
        st.channelMap = { 0, 1, 7, 3, 4, 5, 6, 2 };
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (1024);
        const auto& got = rig.cap->capturedInt;
        bool ok = rig.cap->config.numChannels == 8 && got.size() >= (size_t) frames * 8;
        const int to[6] = { 0, 1, 7, 3, 4, 5 };
        for (int i = 0; ok && i < frames; ++i)
        {
            for (int c = 0; c < 6; ++c) ok = ok && got[(size_t) i * 8 + (size_t) to[c]] == pcm[(size_t) i * 6 + (size_t) c];
            ok = ok && got[(size_t) i * 8 + 2] == 0 && got[(size_t) i * 8 + 6] == 0;
        }
        t.check (ok, "PCM 5.1 sur 8 sorties, centre -> sortie 8 : chaque échantillon à sa place, bit-perfect, sorties libres muettes"_t);
    }

    // 3. DSD (SACD multichannel area, native DSD) with the same map: the bytes move, unused outputs carry DSD silence
    {
        using namespace sacdfixture;
        std::vector<AreaSpec> areas { { 2, { { 4, "Stereo", "F" } } }, { 6, { { 3, "Surround", "F" } } } };
        const auto img = build (areas, "Routage", "F");
        const auto iso = t.tempDir.getChildFile ("route117.iso");
        iso.replaceWithData (img.data(), img.size());
        auto item = EngineRig::item (iso);
        item.uri = nx::sacd::virtualUri (iso, 1, 1);
        const auto ref = expected (1, 1, 6, 3);

        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; c.capabilities.outputChannels = 8; });
        auto st = rig.engine->getSettings();
        st.channelMap = { 0, 1, 7, 3, 4, 5, 6, 2 };
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ item }, 0, true);
        rig.runToEnd (512);
        const auto& got = rig.cap->capturedDsd;
        const size_t frames = ref.size() / 6;
        bool ok = rig.cap->config.numChannels == 8 && got.size() >= frames * 8;
        const int to[6] = { 0, 1, 7, 3, 4, 5 };
        for (size_t i = 0; ok && i < frames; ++i)
        {
            for (int c = 0; c < 6; ++c) ok = ok && got[i * 8 + (size_t) to[c]] == ref[i * 6 + (size_t) c];
            ok = ok && got[i * 8 + 2] == 0x69 && got[i * 8 + 6] == 0x69;
        }
        t.check (ok, "SACD 5.1 en DSD natif sur 8 sorties, centre -> sortie 8 : octets DSD à leur place, sorties libres en silence DSD"_t);

        // 4. "Stéréo" on the same 8-output device: the stereo area, bit-perfect; "Multicanal": the 6 channels
        EngineRig two ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; c.capabilities.outputChannels = 8; });
        auto s2 = two.engine->getSettings();
        s2.channelMode = nx::ChannelMode::Stereo;
        two.engine->setSettings (s2);
        two.engine->setQueue ({ item }, 0, true);
        two.runToEnd (512);
        const auto stereoRef = expected (0, 1, 2, 4);
        const auto& g2 = two.cap->capturedDsd;
        t.check (two.cap->config.numChannels == 2 && two.engine->getCurrentPlan().stereoLayer && g2.size() >= stereoRef.size()
                 && std::equal (stereoRef.begin(), stereoRef.end(), g2.begin()),
                 "mode Stéréo sur une interface 8 sorties : la couche stéréo du SACD, au bit près"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (118, "Sous-étape G : modulateur PCM -> DSD (bruit dans la bande, stabilité à 0 dBFS, charge CPU)")
{
    constexpr double dxd = 352800.0;
    // in-band noise: coherent tone (exact FFT bin, rectangular window), DSD decoded back by the DSD -> DXD converter
    auto measure = [&] (int multiple, int order, double amplitude, double& snrDb, double& noiseDbfs, std::uint64_t& resets)
    {
        constexpr int N = 1 << 17;                                     // 0.37 s at 352.8 kHz
        const int settle = 8192;
        const double f0 = 1000.0 * N / dxd;                          // ~ 1 kHz, on a bin
        const double bin = std::round (f0), freq = bin * dxd / N;
        nx::dsp::PcmToDsd mod;
        mod.prepare (dxd, 2822400.0 * multiple / 64, 2, order, 0.0);
        nx::dsp::DsdToPcm back;
        back.prepare (2822400.0 * multiple / 64, dxd, 2, 1.0);
        const int total = N + settle + mod.getLatencyInputFrames() + 2048;
        std::vector<double> in ((size_t) total * 2);
        for (int i = 0; i < total; ++i) in[(size_t) i * 2] = in[(size_t) i * 2 + 1] = amplitude * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / dxd);
        std::vector<uint8_t> bits ((size_t) total * (size_t) mod.getBytesPerInputFrame() * 2);
        const int bytes = mod.process (in.data(), total, bits.data());
        std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
        const int got = back.process (bits.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
        // Kaiser beta 20 (side lobes ~ -190 dB): the shaped noise above 20 kHz is ~130 dB stronger than in the band,
        // a rectangular window would leak it into the 20 Hz - 20 kHz bins
        std::vector<std::complex<double>> a ((size_t) N);
        const double i0b = nx::dsp::besselI0 (20.0);
        for (int i = 0; i < N; ++i)
        {
            const double r = (2.0 * i - (N - 1)) / (N - 1);
            a[(size_t) i] = pcm[(size_t) (got - N - 1024 + i) * 2] * nx::dsp::besselI0 (20.0 * std::sqrt (1.0 - r * r)) / i0b;
        }
        nx::dsp::fft (a, false);
        double sig = 0.0, noise = 0.0;
        for (int k = 1; k < N / 2; ++k)
        {
            const double hz = k * dxd / N, p = std::norm (a[(size_t) k]);
            if (std::abs (k - bin) <= 12) sig += p;                   // main lobe of the window
            else if (hz >= 20.0 && hz <= 20000.0) noise += p;
        }
        snrDb = 10.0 * std::log10 (sig / noise);
        // noise referred to PCM full scale: PCM 1.0 = 0.5 on the DSD side (SACD 0 dB)
        noiseDbfs = 10.0 * std::log10 (noise * 2.0 / ((double) N * N)) - 20.0 * std::log10 (0.5);
        resets = mod.loopResets;
    };

    juce::String table;
    bool floors = true;
    for (int multiple : { 64, 128, 256 })
        for (int order : { 5, 7 })
        {
            double snr = 0, floor = 0;
            std::uint64_t resets = 0;
            measure (multiple, order, 0.5 * std::pow (10.0, -1.0 / 20.0), snr, floor, resets);   // -1 dBFS (SACD -1 dB)
            table << "DSD" << multiple << "/" << order << " : " << juce::String (snr, 1) << " dB ; ";
            floors = floors && resets == 0 && snr > (order == 7 ? 125.0 : 105.0) + (multiple > 64 ? 10.0 : 0.0);
        }
    t.check (floors, "plancher de bruit 20 Hz - 20 kHz, sinus 1 kHz à -1 dBFS, rapport signal / bruit : "_t + table);

    // stability at 0 dBFS and above: a full-scale sine with 0 dB headroom, and +3 dB inter-sample peaks (fs/4 at 45°)
    {
        nx::dsp::Resampler up;
        up.prepare (44100.0, dxd, 2);
        const int n44 = 44100 * 2;
        std::vector<double> src ((size_t) n44 * 2);
        for (int i = 0; i < n44; ++i)
        {
            const double v = i < n44 / 2 ? std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / 44100.0)
                                         : std::sqrt (2.0) * std::sin (juce::MathConstants<double>::pi * 0.5 * i + juce::MathConstants<double>::pi * 0.25);
            src[(size_t) i * 2] = src[(size_t) i * 2 + 1] = juce::jlimit (-1.0, 1.0, v);   // samples at +/-1.0, true peak +3 dB
        }
        std::vector<double> x ((size_t) (up.maxOutputFor (n44) + 8) * 2);
        const int nX = up.process (src.data(), n44, x.data(), up.maxOutputFor (n44));
        double truePeak = 0.0;
        for (int i = 0; i < nX * 2; ++i) truePeak = juce::jmax (truePeak, std::abs (x[(size_t) i]));

        nx::dsp::PcmToDsd mod;
        mod.prepare (dxd, 2822400.0, 2, 7, 0.0);
        std::vector<uint8_t> bits ((size_t) nX * 8 * 2);
        const int bytes = mod.process (x.data(), nX, bits.data());
        nx::dsp::DsdToPcm back;
        back.prepare (2822400.0, dxd, 2, 1.0);
        std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
        const int got = back.process (bits.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
        // peak of the music only: the decoded stream still carries the shaped noise above 20 kHz, so band-limit first
        nx::dsp::Resampler down;
        down.prepare (dxd, 44100.0, 2);
        std::vector<double> audio ((size_t) (down.maxOutputFor (got) + 8) * 2);
        const int na = down.process (pcm.data(), got, audio.data(), down.maxOutputFor (got));
        nx::dsp::Resampler up2;                                                    // and look between the samples again
        up2.prepare (44100.0, dxd, 2);
        std::vector<double> fine ((size_t) (up2.maxOutputFor (na) + 8) * 2);
        const int nf = up2.process (audio.data(), na, fine.data(), up2.maxOutputFor (na));
        double outPeak = 0.0;
        for (int i = 8192; i < nf * 2; ++i) outPeak = juce::jmax (outPeak, std::abs (fine[(size_t) i]));

        nx::dsp::PcmToDsd quiet;                                                   // default -3 dB: 0 dBFS passes untouched
        quiet.prepare (dxd, 2822400.0, 2, 7, -3.0);
        std::vector<uint8_t> bits2 ((size_t) nX * 8 * 2);
        quiet.process (x.data(), nX / 2, bits2.data());                            // the 997 Hz half: samples at 0 dBFS
        t.check (truePeak > 1.35 && mod.loopResets == 0 && mod.limitedFrames > 0 && outPeak < nx::dsp::PcmToDsd::ceiling + 0.005
                 && quiet.loopResets == 0 && quiet.limitedFrames == 0,
                 "stabilité : 0 dBFS sans marge et crêtes inter-échantillons à "_t + juce::String (20.0 * std::log10 (truePeak), 2)
                 + " dBFS -> limiteur, 0 réinitialisation du modulateur, crête DSD " + juce::String (outPeak, 3) + " (plafond 0,45) ; à -3 dB : limiteur inactif"_t);
    }

    // real-time: no allocation in process(), and the time it takes
    {
        juce::String cpu;
        bool fast = true, noAlloc = true;
        for (int multiple : { 64, 128, 256 })
        {
            nx::dsp::PcmToDsd mod;
            mod.prepare (dxd, 2822400.0 * multiple / 64, 2, 7, -3.0);
            const int block = 4096, blocks = (int) (dxd * 2.0 / block);           // 2 s of stereo DXD
            std::vector<double> in ((size_t) block * 2);
            for (int i = 0; i < block * 2; ++i) in[(size_t) i] = 0.3 * std::sin (0.01 * i);
            std::vector<uint8_t> out ((size_t) block * (size_t) mod.getBytesPerInputFrame() * 2);
            nxtest_allocationCount = 0;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < blocks; ++b)
            {
                nxtest_countAllocations = true;
                mod.process (in.data(), block, out.data());
                nxtest_countAllocations = false;
            }
            const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            noAlloc = noAlloc && nxtest_allocationCount.load() == 0;
            const double load = secs / 2.0 * 100.0;                                // % of one core for real time
            cpu << "DSD" << multiple << " " << juce::String (load, 1) << " %  ";
            fast = fast && load < 100.0;
        }
        t.check (noAlloc && fast, "temps réel, stéréo, 7e ordre : 0 allocation ; charge d'un cœur : "_t + cpu);
    }

    // through the engine: PCM file -> modulator -> DSD (native / DoP) on the virtual DAC
    auto sineWav = [&] (double rate, const juce::String& name)
    {
        const int frames = (int) (rate * 1.2);
        std::vector<int32_t> pcm ((size_t) frames * 2);
        for (int i = 0; i < frames; ++i)
            pcm[(size_t) i * 2] = pcm[(size_t) i * 2 + 1] = (int32_t) std::lround (0.5 * 8388607.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate)) * 256;
        auto f = t.tempDir.getChildFile (name);
        writeWav (f, &pcm, nullptr, 2, (int) rate, 24, 24, false);
        return f;
    };
    // amplitude of the 1 kHz tone in decoded DSD (least squares over 0.25 s in the middle)
    auto toneLevel = [&] (const std::vector<uint8_t>& dsd, double dsdRate)
    {
        nx::dsp::DsdToPcm back;
        back.prepare (dsdRate, dxd, 2, 1.0);
        const int bytes = (int) (dsd.size() / 2);
        std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
        const int got = back.process (dsd.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
        double ss = 0, sc = 0;
        int start = 0;                                                       // the music starts after the DoP pre-roll
        while (start < got && std::abs (pcm[(size_t) start * 2]) < 0.05) ++start;
        const int from = start + 35280, len = 88200;                         // 0.1 s in, 0.25 s long
        if (from + len > got) return 0.0;
        for (int i = from; i < from + len; ++i)
        {
            const double ph = 2.0 * juce::MathConstants<double>::pi * 1000.0 * i / dxd;
            ss += pcm[(size_t) i * 2] * std::sin (ph);
            sc += pcm[(size_t) i * 2] * std::cos (ph);
        }
        return 2.0 * std::sqrt (ss * ss + sc * sc) / len;
    };
    auto withDsd = [] (int target) { return [target] (nx::PlaybackEngine& e) { auto st = e.getSettings(); st.pcmToDsd = target; e.setSettings (st); }; };
    const double expectedLevel = 0.5 * 0.5 * std::pow (10.0, -3.0 / 20.0);   // -6 dBFS PCM -> SACD -6 dB, -3 dB margin
    {
        const auto f = sineWav (44100.0, "g_native.wav");
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0 }; });
        withDsd (128) (*rig.engine);
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        nx::SignalPath path;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (2048, 60.0, [&] (int block) { if (block == 20) path = rig.engine->getSignalPath(); });
        rig.cap->renderFlag = nullptr;
        const double level = toneLevel (rig.cap->capturedDsd, 5644800.0);
        bool node = false;
        for (auto& n : path.nodes) node = node || (n.title.contains ("DSD128") && n.detail.contains ("Sigma"));
        t.check (rig.cap->config.wire == nx::WireFormat::NativeDsd && rig.cap->config.sampleRate == 5644800.0 && node
                 && std::abs (level - expectedLevel) < 0.003 && nxtest_allocationCount.load() == 0,
                 "moteur : WAV 44,1 kHz -> DSD128 natif, niveau "_t + juce::String (20.0 * std::log10 (level / 0.5), 2)
                 + " dB SACD (attendu -9,0 : -6 dBFS + marge -3 dB), étape dans le chemin du signal, 0 allocation sur le thread audio"_t);
    }
    {
        const auto f = sineWav (48000.0, "g_dop48.wav");
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 48000.0, 176400.0, 352800.0 }; });
        withDsd (256) (*rig.engine);                                                // 705,6 kHz DoP absent: DSD128
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (2048);
        const auto& got = rig.cap->capturedInt;
        bool markers = got.size() > 20000;
        for (size_t i = 2; markers && i < 20000; i += 2)
        {
            const auto m = (uint8_t) ((uint32_t) got[i] >> 24), prev = (uint8_t) ((uint32_t) got[i - 2] >> 24);
            markers = (m == 0x05 || m == 0xFA) && m != prev;
        }
        std::vector<uint8_t> dsd;
        for (size_t i = 0; i + 1 < got.size(); i += 2)                             // DoP word: 2 DSD bytes, oldest in bits 16-23
            for (int byteSel : { 16, 8 })
                for (int c = 0; c < 2; ++c) dsd.push_back ((uint8_t) ((uint32_t) got[i + (size_t) c] >> byteSel));
        const double level = toneLevel (dsd, 5644800.0);
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->config.sampleRate == 352800.0 && markers
                 && std::abs (level - expectedLevel) < 0.003 && rig.engine->getCurrentPlan().reason.contains ("DSD256"),
                 "moteur : WAV 48 kHz, DSD256 demandé, DAC limité au DoP 352,8 kHz -> DSD128 en DoP (marqueurs 0x05 / 0xFA), niveau "_t
                 + juce::String (20.0 * std::log10 (level / 0.5), 2) + " dB SACD"_t
);
    }
    {
        const auto f = sineWav (44100.0, "g_pcm.wav");
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.sampleRates = { 44100.0, 48000.0 }; });
        withDsd (64) (*rig.engine);
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (2048);
        t.check (rig.cap->config.wire == nx::WireFormat::Pcm && rig.engine->getCurrentPlan().conversion == nx::PlaybackPlan::Conversion::None
                 && rig.engine->getCurrentPlan().reason.contains ("n'accepte pas le DSD"),
                 "DAC sans DSD : le PCM reste bit-perfect, la raison est affichée"_t);
    }
}

//==============================================================================
namespace
{
    /** A worst-case "EQ" for the tests: +6 dB everywhere (or unity, still "active"). */
    struct TestGain final : nx::DspProcessor
    {
        explicit TestGain (double g) : gain (g) {}
        juce::String getName() const override { return gain > 1.0 ? "Test EQ +6 dB" : "Test EQ 0 dB"; }
        bool prepare (double, int, int) override { return true; }
        void process (double* const* ch, int numCh, int n) noexcept override { for (int c = 0; c < numCh; ++c) for (int i = 0; i < n; ++i) ch[c][i] *= gain; }
        double gain;
    };
    std::unique_ptr<nx::DspChain> testChain (double gain)
    {
        auto c = std::make_unique<nx::DspChain>();
        c->processors.push_back (std::make_unique<TestGain> (gain));
        return c;
    }
}

NX_TEST_SECTION (119, "Sous-étape H : DSD -> DXD 64 bits -> DSP -> modulateur DSD (sortie DSD conservée)")
{
    // 1. the plan's rules
    nx::StreamFormat dsd64; dsd64.kind = nx::SampleKind::Dsd; dsd64.sampleRate = 2822400.0; dsd64.numChannels = 2; dsd64.bitsPerSample = 1;
    nx::StreamFormat dsd256 = dsd64; dsd256.sampleRate = 11289600.0;
    nx::StreamFormat dsd6 = dsd64; dsd6.numChannels = 6;
    nx::StreamFormat pcm; pcm.kind = nx::SampleKind::PcmInt; pcm.sampleRate = 44100.0; pcm.bitsPerSample = 24;
    nx::DeviceCapabilities dop; dop.sampleRates = { 44100.0, 176400.0, 352800.0 }; dop.outputChannels = 2;
    nx::DeviceCapabilities native = dop; native.nativeDsdRates = { 2822400.0, 5644800.0 };
    nx::EngineSettings s;
    s.dspNeedsPcm = true;
    const auto a = nx::makePlan (dsd64, dop, s);
    const auto b = nx::makePlan (dsd256, dop, s);           // DoP 705,6 kHz absent: DSD128
    const auto c = nx::makePlan (dsd64, native, s);
    s.channelMode = nx::ChannelMode::Stereo;
    s.dspNeedsPcm = false;
    const auto mix = nx::makePlan (dsd6, dop, s);           // 5.1 -> 2.0 needs DXD, the output stays DSD
    s.channelMode = nx::ChannelMode::Auto;
    s.dspNeedsPcm = true;
    s.pcmToDsd = 64;
    const auto p = nx::makePlan (pcm, dop, s);              // PCM -> DSD no longer paused by the DSP
    s.pcmToDsd = 0;
    s.keepDsdOutput = false;
    const auto off = nx::makePlan (dsd64, dop, s);
    using Cv = nx::PlaybackPlan::Conversion;
    t.check (a.conversion == Cv::DsdRemodulate && a.wire == nx::WireFormat::DoP && a.deviceRate == 176400.0
             && b.conversion == Cv::DsdRemodulate && b.conversionRate == 5644800.0 && b.deviceRate == 352800.0
             && c.conversion == Cv::DsdRemodulate && c.wire == nx::WireFormat::NativeDsd && c.deviceRate == 2822400.0
             && mix.conversion == Cv::DsdRemodulate && mix.downmix && mix.channels == 2 && mix.wire == nx::WireFormat::DoP
             && p.conversion == Cv::PcmToDsd && p.wire == nx::WireFormat::DoP
             && off.conversion == Cv::DsdToPcm && off.wire == nx::WireFormat::Pcm,
             "plan : DSD + DSP -> remodulé (DoP / natif, DSD256 -> DSD128 si besoin), 5.1 -> 2.0 en DSD, PCM -> DSD avec DSP, option désactivée -> DXD"_t);

    // decoded tone level of a DoP capture
    auto dopToDsd = [] (const std::vector<int32_t>& got)
    {
        std::vector<uint8_t> dsd;
        for (size_t i = 0; i + 1 < got.size(); i += 2)
            for (int byteSel : { 16, 8 })
                for (int ch = 0; ch < 2; ++ch) dsd.push_back ((uint8_t) ((uint32_t) got[i + (size_t) ch] >> byteSel));
        return dsd;
    };
    auto toneLevel = [] (const std::vector<uint8_t>& dsd, double dsdRate, double freq)
    {
        nx::dsp::DsdToPcm back;
        back.prepare (dsdRate, 352800.0, 2, 1.0);
        const int bytes = (int) (dsd.size() / 2);
        std::vector<double> pcmOut ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
        const int got = back.process (dsd.data(), bytes, pcmOut.data(), back.maxOutputFor (bytes));
        int start = 0;
        while (start < got && std::abs (pcmOut[(size_t) start * 2]) < 0.05) ++start;
        const int from = start + 35280, len = 70560;
        if (from + len > got) return 0.0;
        double ss = 0, sc = 0;
        for (int i = from; i < from + len; ++i)
        {
            const double ph = 2.0 * juce::MathConstants<double>::pi * freq * i / 352800.0;
            ss += pcmOut[(size_t) i * 2] * std::sin (ph);
            sc += pcmOut[(size_t) i * 2] * std::cos (ph);
        }
        return 2.0 * std::sqrt (ss * ss + sc * sc) / len;
    };

    // 2. a DSD64 file at SACD 0 dB through a +6 dB "EQ": DSD out (DoP), limited, never unstable, nothing allocated
    {
        const double sacd0 = 0.5;
        auto dsf = t.tempDir.getChildFile ("h_eq.dsf");
        writeDsf (dsf, dsdSine (2822400.0, 1.2, 2, 1000.0, sacd0), 2, 2822400.0, true);
        EngineRig rig ([] (nx::CaptureBackend& cap) { cap.capabilities.sampleRates = { 44100.0, 176400.0 }; });
        auto st = rig.engine->getSettings();
        st.dspNeedsPcm = true;                                 // what the app sets while AURA DSP / Studio Tools are on
        rig.engine->setSettings (st);
        rig.engine->setDecoderDspChain (testChain (2.0));
        rig.engine->setDspChain (testChain (2.0));            // the audio thread's copy (shown in the Signal Path)
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        nx::SignalPath path;
        size_t maxLead = 0;
        rig.engine->setQueue ({ EngineRig::item (dsf) }, 0, true);
        rig.runToEnd (2048, 60.0, [&] (int block)
        {
            if (block == 40) path = rig.engine->getSignalPath();
            if (block > 200) maxLead = juce::jmax (maxLead, rig.engine->getBufferedBytes());
        }, 1u << 16);                                          // the decoder keeps only ~0.5 s ahead here
        rig.cap->renderFlag = nullptr;
        const double level = toneLevel (dopToDsd (rig.cap->capturedInt), 2822400.0, 1000.0);
        bool conv = false, dspNode = false, modNode = false;
        for (auto& n : path.nodes)
        {
            conv |= n.stage == "Conversion" && n.title.contains ("DXD");
            dspNode |= n.stage == "DSP" && n.title.contains ("Test EQ");
            modNode |= n.stage == "Modulateur" && n.title.contains ("DSD64");
        }
        const auto& stx = rig.engine->getStats();
        const size_t leadLimit = (size_t) (176400.0 * 2.0 * 0.9) * 2 + 65536;     // 0.9 s of DoP bytes (+ one chunk)
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.engine->getCurrentPlan().conversion == nx::PlaybackPlan::Conversion::DsdRemodulate
                 && level > 0.43 && level < 0.455 && stx.modulatorResets.load() == 0 && stx.modulatorLimited.load() > 0
                 && conv && dspNode && modNode && path.overall == nx::PathQuality::Enhanced && nxtest_allocationCount.load() == 0,
                 "DSD64 à SACD 0 dB + EQ +6 dB -> DSD64 DoP : limiteur, crête "_t + juce::String (level, 3) + " (plafond 0,45), 0 réinitialisation, "
                 + "chemin du signal DSD -> DXD -> DSP -> modulateur « Amélioré », 0 allocation sur le thread audio"_t);
        t.check (maxLead > 0 && maxLead <= leadLimit,
                 "DSP actif avant le modulateur : avance du décodeur limitée à "_t + juce::String ((double) maxLead / (176400.0 * 2.0 * 2.0), 2)
                 + " s (au lieu de ~6 s) : un réglage d'EQ s'entend tout de suite"_t);
    }

    // 3. PCM -> DSD with the DSP on: the chain applies before the modulator (unity "EQ": the level of G, -9 dB SACD)
    {
        const int frames = (int) (44100 * 1.2);
        std::vector<int32_t> pcmData ((size_t) frames * 2);
        for (int i = 0; i < frames; ++i)
            pcmData[(size_t) i * 2] = pcmData[(size_t) i * 2 + 1] = (int32_t) std::lround (0.5 * 8388607.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0)) * 256;
        auto wav = t.tempDir.getChildFile ("h_pcm.wav");
        writeWav (wav, &pcmData, nullptr, 2, 44100, 24, 24, false);
        EngineRig rig ([] (nx::CaptureBackend& cap) { cap.capabilities.nativeDsdRates = { 2822400.0 }; });
        auto st = rig.engine->getSettings();
        st.dspNeedsPcm = true;
        st.pcmToDsd = 64;
        rig.engine->setSettings (st);
        rig.engine->setDecoderDspChain (testChain (2.0));
        rig.engine->setQueue ({ EngineRig::item (wav) }, 0, true);
        rig.runToEnd (2048, 60.0, {}, 1u << 16);
        const double level = toneLevel (rig.cap->capturedDsd, 2822400.0, 1000.0);
        const double expected = 2.0 * 0.5 * 0.5 * std::pow (10.0, -3.0 / 20.0);      // -6 dBFS, +6 dB EQ, -3 dB margin
        t.check (rig.cap->config.wire == nx::WireFormat::NativeDsd && std::abs (level - expected) < 0.004 && rig.engine->getStats().modulatorResets.load() == 0,
                 "PCM -> DSD64 natif avec DSP actif (plus de pause) : EQ appliqué avant le modulateur, niveau "_t
                 + juce::String (20.0 * std::log10 (level / 0.5), 2) + " dB SACD (attendu -3,0)"_t);
    }

    // 4. a 6-channel DSF on a stereo DoP DAC: ITU 5.1 -> 2.0 in DXD, then DSD again (no DSP involved)
    {
        auto dsf = t.tempDir.getChildFile ("h_six.dsf");
        writeDsf (dsf, dsdSine (2822400.0, 1.2, 6, 1000.0, 0.2), 6, 2822400.0, true);   // channel c: phase c x 0.3 rad
        EngineRig rig ([] (nx::CaptureBackend& cap) { cap.capabilities.sampleRates = { 44100.0, 176400.0 }; cap.capabilities.outputChannels = 2; });
        rig.engine->setQueue ({ EngineRig::item (dsf) }, 0, true);
        rig.runToEnd (2048);
        const double level = toneLevel (dopToDsd (rig.cap->capturedInt), 2822400.0, 1000.0);
        // left = g (FL + 0.707 C + 0.707 SL), FL / C / SL at phases 0, 0.6, 1.2 rad; DSD -> DSD keeps SACD scale, x margin
        const double k = 0.70710678118654752, g = 1.0 / (1.0 + 2.0 * k);
        const double mixAmp = g * std::abs (std::complex<double> (1.0) + k * std::polar (1.0, 0.6) + k * std::polar (1.0, 1.2));
        const double expected = 0.2 * mixAmp * std::pow (10.0, -3.0 / 20.0);
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.cap->config.numChannels == 2 && rig.engine->getCurrentPlan().downmix
                 && rig.engine->getCurrentPlan().conversion == nx::PlaybackPlan::Conversion::DsdRemodulate && std::abs (level - expected) < 0.004,
                 "DSF 6 canaux sur un DAC stéréo DoP : mixage ITU en DXD puis DSD64 en DoP, niveau gauche "_t + juce::String (level, 4)
                 + " (attendu " + juce::String (expected, 4) + ")");
    }
}

//==============================================================================
namespace
{
    struct CountingReset final : nx::DspProcessor
    {
        juce::String getName() const override { return "Test reset"; }
        bool prepare (double, int, int) override { return true; }
        void process (double* const*, int, int) noexcept override {}
        void reset() noexcept override { ++resets; }
        int resets = 0;
    };
}

NX_TEST_SECTION (120, "Transitions sans pop : micro-fondus de 10 ms, remise à zéro du DSP, fréquence fixe en Bluetooth")
{
    // 997 Hz sine at -6 dBFS: never on a zero crossing at a cut, so a hard cut would jump by up to 0.5
    auto sine = [&] (double rate, double seconds, const juce::String& name, double freq = 997.0)
    {
        const int frames = (int) (rate * seconds);
        std::vector<int32_t> pcm ((size_t) frames * 2);
        for (int i = 0; i < frames; ++i)
            pcm[(size_t) i * 2] = pcm[(size_t) i * 2 + 1] = (int32_t) std::lround (0.5 * 8388607.0 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / rate)) * 256;
        auto f = t.tempDir.getChildFile (name);
        writeWav (f, &pcm, nullptr, 2, (int) rate, 24, 24, false);
        return f;
    };
    // largest step between two consecutive samples of the left channel, and where it happens
    auto maxJump = [] (const std::vector<int32_t>& got, size_t& at)
    {
        double m = 0.0;
        for (size_t i = 2; i < got.size(); i += 2)
        {
            const double d = std::abs ((double) got[i] - (double) got[i - 2]) / 2147483648.0;
            if (d > m) { m = d; at = i / 2; }
        }
        return m;
    };
    const double natural = 0.5 * 2.0 * juce::MathConstants<double>::pi * 997.0 / 44100.0;   // a 997 Hz sine's own steepest step
    const double bound = natural * 1.25 + 0.005;

    auto withFades = [] (EngineRig& r) { auto st = r.engine->getSettings(); st.transitionFades = true; r.engine->setSettings (st); };
    const auto a = sine (44100.0, 3.0, "xf_a.wav");
    const auto b = sine (44100.0, 3.0, "xf_b.wav", 613.0);

    // 1. skip in the middle of the music: 10 ms out, silence, 10 ms in; the DSP forgets the old track at gain 0
    {
        EngineRig rig;
        withFades (rig);
        auto chain = std::make_unique<nx::DspChain>();
        auto* counter = new CountingReset();
        chain->processors.emplace_back (counter);
        rig.engine->setDspChain (std::move (chain));
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        rig.engine->setQueue ({ EngineRig::item (a), EngineRig::item (b) }, 0, true);
        size_t skipAt = 0;
        rig.runToEnd (256, 60.0, [&] (int block) { if (block == 120) { skipAt = rig.cap->capturedInt.size() / 2; rig.engine->next(); } });
        rig.cap->renderFlag = nullptr;
        const auto& got = rig.cap->capturedInt;
        size_t at = 0;
        const double jump = maxJump (got, at);
        // shape of the fade: full level 25 ms before the silence, almost nothing in its last ms
        size_t silence = skipAt;
        while (silence < got.size() / 2 && std::abs ((double) got[silence * 2]) / 2147483648.0 > 1e-7) ++silence;
        double before = 0.0, lastMs = 0.0;
        for (size_t i = silence - 1500; i < silence - 1000; ++i) before = juce::jmax (before, std::abs ((double) got[i * 2]) / 2147483648.0);
        for (size_t i = silence - 40; i < silence; ++i) lastMs = juce::jmax (lastMs, std::abs ((double) got[i * 2]) / 2147483648.0);
        const double fadeMs = (double) (silence - skipAt) / 44.1;
        t.check (jump < bound && before > 0.45 && lastMs < 0.03 && fadeMs < 30.0 && counter->resets == 1 && nxtest_allocationCount.load() == 0,
                 "skip en pleine musique : fondu de sortie (plein niveau -> "_t + juce::String (lastMs, 3) + " dans la dernière ms), plus grand saut "
                 + juce::String (jump, 4) + " (sinus seul : " + juce::String (natural, 4) + "), DSP remis à zéro une fois au gain 0, 0 allocation"_t);
    }

    // 2. pause / play, 3. seek: same smoothness
    {
        EngineRig rig;
        withFades (rig);
        rig.engine->setQueue ({ EngineRig::item (a) }, 0, true);
        rig.runToEnd (256, 60.0, [&] (int block)
        {
            if (block == 60) rig.engine->pause();
            if (block == 90) rig.engine->play();
            if (block == 150) rig.engine->seek (2.0);
        });
        size_t at = 0;
        const double jump = maxJump (rig.cap->capturedInt, at);
        t.check (jump < bound, "pause, reprise puis saut à 2 s : aucun saut au-delà de celui du sinus ("_t + juce::String (jump, 4) + ")");
    }

    // 4. end of a track into another rate (device reopened): the last 10 ms fade out; 5. a gapless join stays untouched
    {
        const auto end44 = sine (44100.0, 0.5, "xf_end44.wav");
        const auto next96 = sine (96000.0, 0.5, "xf_next96.wav");
        EngineRig rig;
        withFades (rig);
        rig.engine->setQueue ({ EngineRig::item (end44), EngineRig::item (next96) }, 0, true);
        rig.runToEnd (256);
        size_t at = 0;
        const double jump = maxJump (rig.cap->capturedInt, at);

        // bit-exactness without DSP (an active DSP always dithers), the DSP resets with one
        EngineRig gap;
        withFades (gap);
        const auto half1 = sine (44100.0, 0.5, "xf_g1.wav"), half2 = sine (44100.0, 0.5, "xf_g2.wav");
        gap.engine->setQueue ({ EngineRig::item (half1), EngineRig::item (half2) }, 0, true);
        gap.runToEnd (256);
        EngineRig gapDsp;
        withFades (gapDsp);
        auto chain = std::make_unique<nx::DspChain>();
        auto* counter = new CountingReset();
        chain->processors.emplace_back (counter);
        gapDsp.engine->setDspChain (std::move (chain));
        gapDsp.engine->setQueue ({ EngineRig::item (half1), EngineRig::item (half2) }, 0, true);
        gapDsp.runToEnd (256);
        // around the join (the second half of the first file, the whole second one) every sample is the file's own:
        // no fade at a gapless join (the very start of playback has its own 10 ms fade-in)
        const auto& g = gap.cap->capturedInt;
        size_t first = 0;
        while (first < g.size() / 2 && g[first * 2] == 0) ++first;
        const size_t base = first - 1;                                     // file sample 0 is 0
        const int n = 22050;
        bool exact = first > 0 && g.size() / 2 >= base + (size_t) (2 * n);
        for (int i = n / 2; exact && i < n; ++i)
        {
            const auto v = (int32_t) std::lround (0.5 * 8388607.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / 44100.0)) * 256;
            const auto w = (int32_t) std::lround (0.5 * 8388607.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * (i - n / 2) / 44100.0)) * 256;
            exact = g[(base + (size_t) i) * 2] == v && g[(base + (size_t) (n + i - n / 2)) * 2] == w;
        }
        t.check (jump < bound && exact && counter->resets == 0,
                 "fin de piste vers une autre fréquence : fondu des 10 dernières ms (saut "_t + juce::String (jump, 4)
                 + ") ; enchaînement gapless au même format : au bit près, sans fondu ni remise à zéro"_t
                 + " [" + juce::String ((int) exact) + " " + juce::String (counter->resets) + " " + juce::String ((int) first) + " " + juce::String ((int) (g.size() / 2)) + "]");
    }

    // 6. Bluetooth: the rate the system set stays for the whole queue (one opening), every source converted to it
    {
        auto dsf = t.tempDir.getChildFile ("xf_bt.dsf");
        writeDsf (dsf, dsdSine (2822400.0, 0.4, 2, 1000.0, 0.4), 2, 2822400.0, true);
        const auto r96 = sine (96000.0, 0.4, "xf_bt96.wav"), r48 = sine (48000.0, 0.4, "xf_bt48.wav"), r44 = sine (44100.0, 0.4, "xf_bt44.wav");
        auto btCaps = [] (nx::CaptureBackend& c)
        {
            c.capabilities.sampleRates = { 44100.0, 48000.0, 96000.0, 176400.0 };
            c.capabilities.wireless = true;
            c.capabilities.currentRate = 48000.0;
        };
        EngineRig bt (btCaps);
        nx::SignalPath path;
        double maxRate = 0.0;
        bool allPcm = true;
        bt.engine->setQueue ({ EngineRig::item (r44), EngineRig::item (r96), EngineRig::item (r48), EngineRig::item (dsf) }, 0, true);
        bt.runToEnd (512, 60.0, [&] (int block)
        {
            if (block == 10) path = bt.engine->getSignalPath();
            maxRate = juce::jmax (maxRate, bt.cap->config.sampleRate);
            allPcm = allPcm && bt.cap->config.wire == nx::WireFormat::Pcm;
        });
        bool node = false;
        for (auto& nd : path.nodes) node = node || (nd.stage == nx::tr ("Fréquence"_t) && nd.title.contains ("48"));
        EngineRig wired (btCaps);
        auto st = wired.engine->getSettings();
        st.rateMode = nx::RateMode::FollowSource;
        wired.engine->setSettings (st);
        wired.engine->setQueue ({ EngineRig::item (r44), EngineRig::item (r96), EngineRig::item (r48) }, 0, true);
        wired.runToEnd (512);
        t.check (bt.cap->opens == 1 && maxRate == 48000.0 && allPcm && node && wired.cap->opens == 3,
                 "Bluetooth (48 kHz réglé par le système) : 44,1 / 96 / 48 kHz et DSD joués à 48 kHz, une seule ouverture ; « Toujours suivre la source » : 3 ouvertures"_t
                 + " [" + juce::String (bt.cap->opens) + " " + juce::String (maxRate) + " " + juce::String ((int) allPcm) + " " + juce::String ((int) node) + " " + juce::String (wired.cap->opens) + "]");
    }
}

//==============================================================================
NX_TEST_SECTION (121, "Étape 2 : E9DS (de-esser transparent) : bande seule, bit-identique sous le seuil, sans clic, 0 allocation")
{
    constexpr double pi = 3.14159265358979323846;
    auto tone = [&] (std::vector<double>& v, double rate, std::initializer_list<std::pair<double, double>> parts)
    {
        for (size_t i = 0; i < v.size(); ++i)
        {
            double s = 0.0;
            for (auto& [f, a] : parts) s += a * std::sin (2.0 * pi * f * (double) i / rate);
            v[i] = s;
        }
    };
    // runs a stereo signal (L = R) through a fresh processor; returns L
    auto run = [&] (nx::E9dsState::Mode mode, double focus, double amount, double rate, std::vector<double> x, bool count = false)
    {
        auto st = std::make_shared<nx::E9dsState>();
        st->set (true, focus, amount, mode);
        nx::E9dsProcessor p (st);
        p.prepare (rate, 2, 4096);
        auto r = x;
        if (count) { nxtest_allocationCount = 0; nxtest_countAllocations = true; }
        for (size_t i = 0; i < x.size(); i += 512)
        {
            double* ch[2] = { x.data() + i, r.data() + i };
            p.process (ch, 2, (int) std::min<size_t> (512, x.size() - i));
        }
        nxtest_countAllocations = false;
        return x;
    };
    const double fs = 48000.0;
    const int n = 48000;
    const int tail = 24000;                                         // settled half
    auto amp = [&] (const std::vector<double>& v, double f) { return fitSine (v.data() + tail, n - tail, 1, fs, f).first; };
    auto db = [] (double a, double b) { return 20.0 * std::log10 (a / b); };

    // 1. below the threshold (music with little treble): bit-identical, both modes
    {
        std::vector<double> x ((size_t) n);
        tone (x, fs, { { 220.0, 0.4 }, { 1000.0, 0.2 }, { 3000.0, 0.05 }, { 7000.0, 0.01 } });
        bool same = true;
        juce::String where;
        for (auto m : { nx::E9dsState::SplitBand, nx::E9dsState::DynamicHf })
        {
            const auto y = run (m, 6500.0, 6.0, fs, x);
            int first = -1, count = 0;
            for (size_t i = 0; i < x.size(); ++i) if (y[i] != x[i]) { if (first < 0) first = (int) i; ++count; }
            same = same && count == 0;
            where << " " << (int) m << ":" << first << "/" << count;
        }
        t.check (same, "sous le seuil (aigus à -32 dB du signal) : sortie identique à l'entrée au bit près, Split-Band et Dynamic HF"_t + (same ? juce::String() : where));
    }
    // 2. Split-Band: a loud sibilant at 6.5 kHz is cut, capped at Amount; the 1 kHz under it does not move
    {
        std::vector<double> x ((size_t) n);
        tone (x, fs, { { 1000.0, 0.2 }, { 6500.0, 0.4 } });
        const auto y6 = run (nx::E9dsState::SplitBand, 6500.0, 6.0, fs, x, true);
        const long allocs = nxtest_allocationCount.load();
        const auto y3 = run (nx::E9dsState::SplitBand, 6500.0, 3.0, fs, x);
        const double cut6 = db (amp (y6, 6500.0), 0.4), cut3 = db (amp (y3, 6500.0), 0.4), low = db (amp (y6, 1000.0), 0.2);
        t.check (cut6 < -2.0 && cut6 >= -6.01 && std::abs (cut3 + 3.0) < 0.05 && std::abs (low) < 0.05 && allocs == 0,
                 "Split-Band 6,5 kHz : sibilance "_t + juce::String (cut6, 2) + " dB (plafond -6), Amount -3 -> " + juce::String (cut3, 2)
                 + " dB ; 1 kHz dessous : " + juce::String (low, 3) + " dB ; 0 allocation dans process()");
    }
    // 3. Dynamic HF: harsh 12 kHz softened, 1 kHz kept within 0.1 dB
    {
        std::vector<double> x ((size_t) n);
        tone (x, fs, { { 1000.0, 0.2 }, { 12000.0, 0.4 } });
        const auto y = run (nx::E9dsState::DynamicHf, 6500.0, 6.0, fs, x);
        const double hf = db (amp (y, 12000.0), 0.4), low = db (amp (y, 1000.0), 0.2);
        t.check (hf < -2.0 && hf >= -6.01 && std::abs (low) < 0.1,
                 "Dynamic HF au-dessus de 6,5 kHz : 12 kHz "_t + juce::String (hf, 2) + " dB, 1 kHz " + juce::String (low, 3) + " dB");
    }
    // 4. switching off (and on) while it is cutting, at 352.8 kHz: a 20 ms ramp, no step in the output
    {
        const double hr = 352800.0;
        std::vector<double> x ((size_t) hr), r;
        tone (x, hr, { { 6500.0, 0.4 } });
        r = x;
        auto st = std::make_shared<nx::E9dsState>();
        st->set (true, 6500.0, 6.0, nx::E9dsState::SplitBand);
        nx::E9dsProcessor p (st);
        p.prepare (hr, 2, 4096);
        auto out = x;
        double maxStep = 0.0, maxStepIn = 0.0;
        bool bypassedAfter = false;
        for (size_t i = 0; i + 1024 <= x.size(); i += 1024)
        {
            if (i == 100 * 1024) st->enabled = false;
            if (i == 260 * 1024) st->enabled = true;
            if (i == 250 * 1024) bypassedAfter = p.isBypassed();
            if (p.isBypassed()) continue;
            double* ch[2] = { out.data() + i, r.data() + i };
            p.process (ch, 2, 1024);
        }
        for (size_t i = 1; i < x.size(); ++i)
        {
            maxStep = std::max (maxStep, std::abs (out[i] - out[i - 1]));
            maxStepIn = std::max (maxStepIn, std::abs (x[i] - x[i - 1]));
        }
        t.check (maxStep <= maxStepIn * 1.001 && bypassedAfter,
                 "E9DS coupé / remis pendant une réduction (352,8 kHz) : rampe de 20 ms, pas de saut (pas max "_t + juce::String (maxStep / maxStepIn, 4)
                 + " x celui du signal), puis contourné");
    }
    // 5. engine: a DSD64 source with E9DS on stays DSD (remodulated), Signal Path shows E9DS, 0 allocation
    {
        auto dsf = t.tempDir.getChildFile ("e9ds.dsf");
        writeDsf (dsf, dsdSine (2822400.0, 1.0, 2, 1000.0, 0.4), 2, 2822400.0, true);
        EngineRig rig ([] (nx::CaptureBackend& cap) { cap.capabilities.sampleRates = { 44100.0, 176400.0 }; });
        auto st = rig.engine->getSettings();
        st.dspNeedsPcm = true;
        rig.engine->setSettings (st);
        auto e9 = std::make_shared<nx::E9dsState>();
        e9->set (true, 6500.0, 6.0, nx::E9dsState::SplitBand);
        for (const bool dec : { true, false })
        {
            auto chain = std::make_unique<nx::DspChain>();
            chain->processors.push_back (std::make_unique<nx::E9dsProcessor> (e9));
            if (dec) rig.engine->setDecoderDspChain (std::move (chain)); else rig.engine->setDspChain (std::move (chain));
        }
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        nx::SignalPath path;
        rig.engine->setQueue ({ EngineRig::item (dsf) }, 0, true);
        rig.runToEnd (2048, 60.0, [&] (int block) { if (block == 40) path = rig.engine->getSignalPath(); }, 1u << 16);
        rig.cap->renderFlag = nullptr;
        bool node = false;
        for (auto& nd : path.nodes) node |= nd.stage == "DSP" && nd.title.contains ("E9DS");
        t.check (rig.cap->config.wire == nx::WireFormat::DoP && rig.engine->getCurrentPlan().conversion == nx::PlaybackPlan::Conversion::DsdRemodulate
                 && node && nxtest_allocationCount.load() == 0,
                 "moteur : DSD64 + E9DS -> DXD 64 bits -> E9DS -> modulateur -> DSD64 DoP, « E9DS » dans le chemin du signal, 0 allocation sur le thread audio"_t);
    }
}

//==============================================================================
NX_TEST_SECTION (122, "Étape 3 : XZ Automatic (taux DSD selon la source), repli sur le DAC, fondu de sortie avant changement de taux")
{
    using Cv = nx::PlaybackPlan::Conversion;
    // 1. the table, on a DAC that takes everything up to DSD256 natively
    {
        nx::StreamFormat pcm; pcm.kind = nx::SampleKind::PcmInt; pcm.numChannels = 2; pcm.bitsPerSample = 24;
        nx::DeviceCapabilities all; all.sampleRates = { 44100.0, 88200.0, 176400.0, 352800.0, 705600.0 }; all.outputChannels = 2;
        all.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0, 22579200.0 };
        nx::EngineSettings s;
        s.pcmToDsd = nx::EngineSettings::xzAutomatic;
        const std::vector<std::pair<double, int>> table { { 44100.0, 64 }, { 48000.0, 128 }, { 88200.0, 128 }, { 176400.0, 128 },
                                                          { 96000.0, 256 }, { 192000.0, 256 }, { 352800.0, 256 }, { 384000.0, 256 } };
        bool ok = true;
        juce::String got;
        for (auto [rate, mult] : table)
        {
            pcm.sampleRate = rate;
            const auto p = nx::makePlan (pcm, all, s);
            const bool right = p.conversion == Cv::PcmToDsd && p.wire == nx::WireFormat::NativeDsd && p.conversionRate == 2822400.0 * mult / 64
                               && p.xzAuto && p.dsdTarget == mult && p.reason.contains ("XZ Automatic");
            ok = ok && right;
            got << " " << nx::formatRate (rate) << "->DSD" << (int) std::lround (p.conversionRate / 44100.0);
        }
        t.check (ok, "XZ Automatic : 44,1 -> DSD64 ; 48 / 88,2 / 176,4 -> DSD128 ; 96 / 192 -> DSD256 ; 352,8 / 384 -> DSD256 (plafond du moteur)"_t + " [" + got.trim() + "]");

        // 2. DAC limited to DSD128 (native) / to DoP 352,8 kHz: down to DSD128, the target is kept for the display
        nx::DeviceCapabilities d128 = all; d128.nativeDsdRates = { 2822400.0, 5644800.0 }; d128.sampleRates = { 44100.0, 88200.0, 176400.0 };
        nx::DeviceCapabilities dop = all; dop.nativeDsdRates.clear(); dop.sampleRates = { 44100.0, 88200.0, 176400.0, 352800.0 };
        pcm.sampleRate = 96000.0;
        const auto n = nx::makePlan (pcm, d128, s);
        pcm.sampleRate = 192000.0;
        const auto d = nx::makePlan (pcm, dop, s);
        pcm.sampleRate = 44100.0;
        const auto low = nx::makePlan (pcm, d128, s);
        nx::DeviceCapabilities none = all; none.nativeDsdRates.clear(); none.sampleRates = { 44100.0, 48000.0, 96000.0 };
        pcm.sampleRate = 96000.0;
        const auto pcmOnly = nx::makePlan (pcm, none, s);
        t.check (n.conversionRate == 5644800.0 && n.wire == nx::WireFormat::NativeDsd && n.dsdTarget == 256 && n.reason.contains ("DSD256 non accepté par le DAC")
                 && d.conversionRate == 5644800.0 && d.wire == nx::WireFormat::DoP && d.deviceRate == 352800.0
                 && low.conversionRate == 2822400.0 && pcmOnly.wire == nx::WireFormat::Pcm && pcmOnly.conversion == Cv::None,
                 "DAC limité au DSD128 : 96 kHz -> DSD128 natif, 192 kHz -> DSD128 en DoP 352,8 kHz, 44,1 kHz -> DSD64 ; DAC sans DSD : PCM bit-perfect"_t);
    }

    // tone amplitude of channel 0 of an interleaved stereo DSD stream, per 1 ms window (1 kHz sine: one period)
    auto envelope = [] (const std::vector<uint8_t>& dsd, size_t from, size_t to, double rate)
    {
        std::vector<double> env;
        const int win = (int) std::lround (rate / 1000.0);
        std::vector<double> bits;
        for (size_t i = from; i + 1 < to; i += 2)
            for (int b = 7; b >= 0; --b) bits.push_back ((dsd[i] >> b) & 1 ? 1.0 : -1.0);
        for (size_t w = 0; w + (size_t) win <= bits.size(); w += (size_t) win)
        {
            double ss = 0, sc = 0;
            for (int k = 0; k < win; ++k)
            {
                const double ph = 2.0 * juce::MathConstants<double>::pi * k / win;
                ss += bits[w + (size_t) k] * std::sin (ph);
                sc += bits[w + (size_t) k] * std::cos (ph);
            }
            env.push_back (2.0 * std::sqrt (ss * ss + sc * sc) / win);
        }
        return env;
    };
    auto sineWav = [&] (const juce::String& name, double rate, double seconds)
    {
        auto f = t.tempDir.getChildFile (name);
        const auto s = sinePcm ((int) (rate * seconds), 2, 24, rate, 1000.0, 0.5);
        writeWav (f, &s, nullptr, 2, rate, 24, 24, false);
        return f;
    };

    // 3. 44,1 kHz then 96 kHz (DSD64 -> DSD256, native): the end of the first track fades out over 10 ms before the change,
    //    the second one fades in; the modulator never resets; nothing allocated on the audio thread
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0 }; });
        auto st = rig.engine->getSettings();
        st.pcmToDsd = nx::EngineSettings::xzAutomatic;
        st.transitionFades = true;
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ EngineRig::item (sineWav ("xz_a.wav", 44100.0, 0.4)), EngineRig::item (sineWav ("xz_b.wav", 96000.0, 0.3)) }, 0, true);
        size_t split = 0;
        std::vector<double> rates;
        nx::SignalPath pathA, pathB;
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        rig.runToEnd (1024, 60.0, [&] (int)
        {
            const double r = rig.cap->config.sampleRate;
            if (rates.empty() || rates.back() != r) { rates.push_back (r); if (rates.size() == 2) split = rig.cap->capturedDsd.size(); }
            if (rates.size() == 1 && pathA.nodes.empty() && rig.cap->capturedDsd.size() > 200000) pathA = rig.engine->getSignalPath();
            if (rates.size() == 2 && pathB.nodes.empty() && rig.cap->capturedDsd.size() > split + 400000) pathB = rig.engine->getSignalPath();
        });
        rig.cap->renderFlag = nullptr;
        // end of A: the last window above half the tone, then how many windows the fall takes
        const auto envA = envelope (rig.cap->capturedDsd, 0, split > 0 ? split : rig.cap->capturedDsd.size(), 2822400.0);
        double full = 0;
        for (auto v : envA) full = juce::jmax (full, v);
        int last = -1;
        for (int i = 0; i < (int) envA.size(); ++i) if (envA[(size_t) i] > 0.1 * full) last = i;
        int fall = 0;
        for (int i = last; i >= 0 && envA[(size_t) i] < 0.9 * full; --i) ++fall;
        auto modNode = [] (const nx::SignalPath& p)
        {
            for (auto& n : p.nodes) if (n.stage == "Modulateur") return n.title + " | " + n.detail;
            return juce::String();
        };
        const auto a = modNode (pathA), b = modNode (pathB);
        t.check (rates.size() == 2 && rates[0] == 2822400.0 && rates[1] == 11289600.0 && rig.cap->opens == 2,
                 "XZ Automatic à la volée : 44,1 kHz en DSD64 puis 96 kHz en DSD256 (DSD natif), une réouverture du DAC"_t);
        t.check (fall >= 6 && fall <= 14 && rig.engine->getStats().modulatorResets.load() == 0 && nxtest_allocationCount.load() == 0,
                 "fin de la piste 44,1 kHz avant le passage en DSD256 : fondu de sortie de 10 ms en DXD (de 90 % à 10 % en "_t + juce::String (fall) + " ms, sans reprise après), "
                 + "0 réinitialisation du modulateur, 0 allocation sur le thread audio");
        t.check (a.contains ("DXD -> DSD64 · XZ Automatic") && a.contains ("44,1 kHz -> DSD64") && b.contains ("DXD -> DSD256 · XZ Automatic") && b.contains ("96 kHz -> DSD256"),
                 "Chemin du signal : « "_t + a.upToFirstOccurrenceOf (" | ", false, false) + " », puis « " + b.upToFirstOccurrenceOf (" | ", false, false) + " »");
    }

    // 4. DAC limited to DSD128: the display says what was aimed at and what is played
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0 }; });
        auto st = rig.engine->getSettings();
        st.pcmToDsd = nx::EngineSettings::xzAutomatic;
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ EngineRig::item (sineWav ("xz_c.wav", 192000.0, 0.3)) }, 0, true);
        nx::SignalPath path;
        rig.runToEnd (1024, 60.0, [&] (int block) { if (block == 20) path = rig.engine->getSignalPath(); });
        juce::String node;
        for (auto& n : path.nodes) if (n.stage == "Modulateur") node = n.title + " | " + n.detail;
        t.check (rig.cap->config.sampleRate == 5644800.0 && node.contains ("DSD128 · XZ Automatic") && node.contains ("192 kHz -> DSD256")
                 && node.contains ("DSD128 (limite du DAC)"),
                 "DAC limité au DSD128 : 192 kHz joué en DSD128 sans coupure, chemin du signal « "_t + node.upToFirstOccurrenceOf (" · Sigma", false, false) + " »");
    }

    // 5. two 44,1 kHz tracks back to back: same DSD64, one opening, no fade at the join (gapless)
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0 }; });
        auto st = rig.engine->getSettings();
        st.pcmToDsd = nx::EngineSettings::xzAutomatic;
        st.transitionFades = true;
        rig.engine->setSettings (st);
        rig.engine->setQueue ({ EngineRig::item (sineWav ("xz_d.wav", 44100.0, 0.3)), EngineRig::item (sineWav ("xz_e.wav", 44100.0, 0.3)) }, 0, true);
        rig.runToEnd (1024);
        const auto env = envelope (rig.cap->capturedDsd, 0, rig.cap->capturedDsd.size(), 2822400.0);
        double full = 0;
        for (auto v : env) full = juce::jmax (full, v);
        int first = -1, last = -1;
        for (int i = 0; i < (int) env.size(); ++i) if (env[(size_t) i] > 0.5 * full) { if (first < 0) first = i; last = i; }
        double dip = full;
        for (int i = first + 20; i < last - 20; ++i) dip = juce::jmin (dip, env[(size_t) i]);   // the join is at ~300 ms
        t.check (rig.cap->opens == 1 && last - first >= 590 && dip > 0.9 * full,
                 "deux pistes 44,1 kHz enchaînées : DSD64 sans réouverture ni fondu à la jonction (creux "_t + juce::String (20.0 * std::log10 (dip / full), 2) + " dB)");
    }
}

//==============================================================================
NX_TEST_SECTION (123, "E9DS avec la conversion PCM -> DSD (XZ Automatic, DSD128 fixe) : en DXD avant le modulateur")
{
    constexpr double dxd = 352800.0;
    // level of a tone in a stereo DSD capture (channel 0), decoded back to DXD, 0.25 s in the middle of the music
    auto level = [&] (const std::vector<uint8_t>& dsd, double dsdRate, double freq)
    {
        nx::dsp::DsdToPcm back;
        back.prepare (dsdRate, dxd, 2, 1.0);
        const int bytes = (int) (dsd.size() / 2);
        std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
        const int got = back.process (dsd.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
        int start = 0;
        while (start < got && std::abs (pcm[(size_t) start * 2]) < 0.05) ++start;
        const int from = start + 70560, len = 88200;                       // 0.2 s in (E9DS settled), 0.25 s long
        if (from + len > got) return 0.0;
        return fitSine (pcm.data() + (size_t) from * 2, len, 2, dxd, freq).first;
    };
    auto f = t.tempDir.getChildFile ("e9_xz.wav");
    {
        const double rate = 44100.0;
        const int n = (int) (rate * 0.8);
        std::vector<int32_t> pcm ((size_t) n * 2);
        for (int i = 0; i < n; ++i)
        {
            const double x = 0.15 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate)
                           + 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 6500.0 * i / rate);
            pcm[(size_t) i * 2] = pcm[(size_t) i * 2 + 1] = (int32_t) std::llround (x * 2147483647.0) & ~0xff;
        }
        writeWav (f, &pcm, nullptr, 2, rate, 24, 24, false);
    }
    // plays the file to the DSD target with E9DS on / off; returns { 1 kHz, 6.5 kHz, DSD rate, Signal Path DSP node, meter }
    struct Run { double low = 0, high = 0, rate = 0; juce::String dsp; double meter = 0; };
    auto play = [&] (int target, bool e9On)
    {
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0 }; });
        auto st = rig.engine->getSettings();
        st.pcmToDsd = target;
        st.dspNeedsPcm = e9On;                       // what AppController sets while E9DS is on
        rig.engine->setSettings (st);
        auto e9 = std::make_shared<nx::E9dsState>();
        e9->set (e9On, 6500.0, 6.0, nx::E9dsState::SplitBand);
        for (const bool dec : { true, false })       // as AppController::rebuildDspChain
        {
            auto chain = std::make_unique<nx::DspChain>();
            chain->processors.push_back (std::make_unique<nx::E9dsProcessor> (e9));
            if (dec) rig.engine->setDecoderDspChain (std::move (chain)); else rig.engine->setDspChain (std::move (chain));
        }
        Run r;
        rig.engine->setQueue ({ EngineRig::item (f) }, 0, true);
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 300)
            {
                for (auto& nd : rig.engine->getSignalPath().nodes) if (nd.stage == "DSP") r.dsp = nd.title;
                r.meter = e9->reductionDb.load();
            }
        }, 1u << 16);
        r.rate = rig.cap->config.sampleRate;
        r.low = level (rig.cap->capturedDsd, r.rate, 1000.0);
        r.high = level (rig.cap->capturedDsd, r.rate, 6500.0);
        return r;
    };
    for (int target : { nx::EngineSettings::xzAutomatic, 128 })
    {
        const auto off = play (target, false), on = play (target, true);
        const double cut = 20.0 * std::log10 (on.high / off.high), low = 20.0 * std::log10 (on.low / off.low);
        const auto name = target < 0 ? juce::String ("XZ Automatic (DSD64)") : juce::String ("DSD128 fixe"_t);
        t.check (on.rate == (target < 0 ? 2822400.0 : 5644800.0) && cut < -2.0 && cut > -6.1 && std::abs (low) < 0.1
                 && on.dsp.contains ("E9DS") && off.dsp.isEmpty() && on.meter > 1.0,
                 "PCM 44,1 kHz -> "_t + name + " + E9DS : sibilance 6,5 kHz " + juce::String (cut, 2) + " dB, 1 kHz " + juce::String (low, 3)
                 + " dB, « E9DS » dans le chemin du signal, indicateur de réduction " + juce::String (on.meter, 1) + " dB");
    }
}

NX_TEST_SECTION (124, "Auto-Headroom True Peak : mesure BS.1770-4, atténuation par piste, bit-perfect sous le plafond, 0 allocation")
{
    constexpr double pi = juce::MathConstants<double>::pi;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // 1. the meter: a sine at fs/4 sampled at 45° has its samples at 1.0 but peaks at sqrt(2) between them (+3,01 dBTP)
    {
        const int n = 48000;
        std::vector<double> over ((size_t) n * 2), clean ((size_t) n * 2);
        for (int i = 0; i < n; ++i)
        {
            over[(size_t) i * 2] = over[(size_t) i * 2 + 1] = std::sqrt (2.0) * std::sin (pi / 2.0 * i + pi / 4.0);
            clean[(size_t) i * 2] = clean[(size_t) i * 2 + 1] = 0.5 * std::sin (2.0 * pi * 1000.0 * i / 48000.0);
        }
        nx::truepeak::Meter a (2), b (2);
        a.add (over.data(), n);
        b.add (clean.data(), n);
        t.check (std::abs (a.dbtp() - 3.01) < 0.3 && std::abs (b.dbtp() + 6.02) < 0.1,
                 "True Peak ITU-R BS.1770-4 (x4) : sinus fs/4 à 45° (échantillons à 0 dBFS) = "_t + juce::String (a.dbtp(), 2)
                     + " dBTP (attendu +3,01), sinus 1 kHz à -6 dBFS = " + juce::String (b.dbtp(), 2) + " dBTP");
        using nx::truepeak::attenuationDb;
        t.check (attenuationDb (2.0, -1.0) == 3.0 && attenuationDb (-2.3, -1.0) == 0.0 && attenuationDb (nan, -1.0) == 0.0
                     && std::abs (attenuationDb (-0.7, -1.0) - 0.3) < 1e-12 && attenuationDb (-0.7, -0.5) == 0.0
                     && nx::truepeak::gain (false, 5.0, -1.0) == 1.0 && nx::truepeak::gain (true, -3.0, -1.0) == 1.0,
                 "marge : seulement l'excès au-dessus du plafond (-1,0 / -0,5 dBTP), 0 dB pour un morceau propre ou non mesuré"_t);
    }
    // 2. the background analysis of a file measures it (24-bit WAV with inter-sample overs)
    auto overs = t.tempDir.getChildFile ("tp_overs.wav");
    {
        std::vector<int32_t> pcm (96000 * 2);
        for (size_t i = 0; i < pcm.size() / 2; ++i)
            pcm[i * 2] = pcm[i * 2 + 1] = (int32_t) ((uint32_t) (std::llround (0.999 * std::sqrt (2.0) * std::sin (pi / 2.0 * (double) i + pi / 4.0) * 8388608.0)) << 8);
        writeWav (overs, &pcm, nullptr, 2, 96000, 24, 24, false);
        const auto an = nx::TrackAnalyzer::analyse (overs, 400, [] { return false; });
        t.check (an != nullptr && an->truePeakDb > 2.5 && an->peakDb < 0.0 && an->peakDb > -0.1,
                 "analyse en tâche de fond : Peak "_t + (an ? juce::String (an->peakDb, 2) : "?") + " dBFS, True Peak "
                     + (an ? juce::String (an->truePeakDb, 2) : "?") + " dBTP (crêtes inter-échantillons détectées)");
    }
    // 3. library: kept with the file, forgotten when the file changes
    {
        nx::Library lib;
        juce::String err;
        lib.openInMemory (t.tempDir.getChildFile ("tp_lib"), err);
        nx::TrackInfo tr;
        tr.source = "local"; tr.externalId = tr.uri = "/music/a.flac"; tr.title = "A"; tr.fileSize = 1000; tr.modified = 100;
        lib.upsert (tr);
        const bool none = ! lib.getTruePeak (tr.externalId).has_value();
        lib.setTruePeak (tr.externalId, 1.25);
        lib.upsert (tr);                                                   // rescan, same file
        const auto kept = lib.getTruePeak (tr.externalId);
        tr.modified = 200;
        lib.upsert (tr);                                                   // the file changed
        t.check (none && kept.has_value() && *kept == 1.25 && ! lib.getTruePeak (tr.externalId).has_value(),
                 "base : colonne true_peak gardée au rescan, effacée quand le fichier change (taille / date)"_t);
    }

    // 4. engine, PCM to the DAC as is: a loud track (TP +2,0) then a clean one, gapless
    const double g = std::pow (10.0, -3.0 / 20.0);                       // +2,0 dBTP, ceiling -1,0: -3,0 dB
    auto loud = t.tempDir.getChildFile ("tp_loud.wav"), clean = t.tempDir.getChildFile ("tp_clean.wav");
    const auto loudPcm = sinePcm (57600, 2, 24, 96000.0, 1000.0, 0.5);
    const auto cleanPcm = randomPcm (96000, 2, 24, 4242u);
    writeWav (loud, &loudPcm, nullptr, 2, 96000, 24, 24, false);
    writeWav (clean, &cleanPcm, nullptr, 2, 96000, 24, 24, false);
    struct Run { std::vector<int32_t> got; std::vector<uint8_t> dsd; double rate = 0; juce::String node; long allocs = 0; };
    auto play = [&] (bool on, double tpLoud, int liveAt, std::function<void (nx::CaptureBackend&)> cfg = {}, std::function<void (nx::EngineSettings&)> more = {})
    {
        EngineRig rig (cfg);
        auto st = rig.engine->getSettings();
        st.autoTruePeak = on;
        st.truePeakCeilingDb = -1.0;
        if (more) more (st);
        rig.engine->setSettings (st);
        auto a = EngineRig::item (loud), b = EngineRig::item (clean);
        a.truePeakDb = tpLoud;
        b.truePeakDb = -6.0;
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        Run r;
        rig.engine->setQueue ({ a, b }, 0, true);
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 8) for (auto& nd : rig.engine->getSignalPath().nodes) if (nd.stage == "Headroom") r.node = nd.title + " | " + nd.detail;
            if (block == liveAt) rig.engine->setTrackTruePeak (0, 2.0);
        });
        rig.cap->renderFlag = nullptr;
        r.allocs = nxtest_allocationCount.load();
        r.got = rig.cap->capturedInt;
        r.dsd = rig.cap->capturedDsd;
        r.rate = rig.cap->config.sampleRate;
        return r;
    };
    auto amp = [] (const std::vector<int32_t>& v, int from, int len, double rate)
    {
        if ((size_t) (from + len) * 2 > v.size()) return 0.0;
        std::vector<double> x ((size_t) len);
        for (int i = 0; i < len; ++i) x[(size_t) i] = v[(size_t) (from + i) * 2] * (1.0 / 2147483648.0);
        return fitSine (x.data(), len, 1, rate, 1000.0).first;
    };
    auto sameFrom = [&] (const std::vector<int32_t>& got, size_t from, size_t to)
    {
        if (got.size() < to) return false;
        for (size_t i = from; i < to; ++i) if (got[i] != (i < loudPcm.size() ? loudPcm[i] : cleanPcm[i - loudPcm.size()])) return false;
        return true;
    };
    {
        const auto off = play (false, 2.0, -1);
        const auto on = play (true, 2.0, -1);
        const double ratio = amp (on.got, 20000, 30000, 96000.0) / 0.5;
        const size_t end = loudPcm.size() + cleanPcm.size();
        t.check (sameFrom (off.got, 0, end), "désactivé : les deux pistes arrivent au DAC bit à bit (bit-perfect strict)"_t);
        t.check (std::abs (20.0 * std::log10 (ratio) + 3.0) < 0.01 && sameFrom (on.got, loudPcm.size() + 38400, end) && on.allocs == 0,
                 "actif, plafond -1,0 dBTP : piste à +2,0 dBTP baissée de "_t + juce::String (20.0 * std::log10 (ratio), 3)
                     + " dB (attendu -3,000), piste propre suivante bit à bit (gapless, 0 dB), 0 allocation sur le thread audio");
        t.check (on.node.contains ("-3,0 dB") && on.node.contains ("+2,0 dBTP"), "Chemin du signal : "_t + on.node);
        // measurement arriving during playback (first listen): the rest of the track is attenuated, 10 ms glide
        const auto live = play (true, nan, 20);
        const double before = amp (live.got, 2000, 8000, 96000.0) / 0.5, after = amp (live.got, 36000, 20000, 96000.0) / 0.5;
        int maxStep = 0;
        for (size_t i = 2 * 20000; i + 2 < 2 * 36000; i += 2) maxStep = std::max (maxStep, std::abs ((live.got[i + 2] >> 8) - (live.got[i] >> 8)));
        const int sineStep = (int) std::lround (0.5 * 8388608.0 * 2.0 * pi * 1000.0 / 96000.0) + 4;
        t.check (std::abs (before - 1.0) < 1e-3 && std::abs (20.0 * std::log10 (after) + 3.0) < 0.01 && maxStep <= sineStep,
                 "mesure arrivée en cours de lecture : 0 dB avant, "_t + juce::String (20.0 * std::log10 (after), 3) + " dB après, sans saut (pas max "
                     + juce::String (maxStep) + " <= " + juce::String (sineStep) + ")");
    }
    // 5. conversions: the decoder attenuates before the resampler (DXD upsampling) and before the DSD modulator
    {
        auto up = [] (nx::EngineSettings& st) { st.upsampleToDxd = true; };
        const auto off = play (false, 2.0, -1, {}, up), on = play (true, 2.0, -1, {}, up);
        const double rel = 20.0 * std::log10 (amp (on.got, 100000, 80000, on.rate) / juce::jmax (1e-9, amp (off.got, 100000, 80000, off.rate)));
        t.check (on.rate > 300000.0 && std::abs (rel + 3.0) < 0.02 && on.allocs == 0,
                 "suréchantillonnage vers "_t + nx::formatRate (on.rate) + " : atténuation appliquée avant le filtre sinc ("
                     + juce::String (rel, 3) + " dB), 0 allocation");

        auto dsdCaps = [] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0 }; };
        auto toDsd = [] (nx::EngineSettings& st) { st.pcmToDsd = 128; };
        auto level = [&] (const std::vector<uint8_t>& dsd, double dsdRate)
        {
            constexpr double dxd = 352800.0;
            nx::dsp::DsdToPcm back;
            back.prepare (dsdRate, dxd, 2, 1.0);
            const int bytes = (int) (dsd.size() / 2);
            std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
            const int got = back.process (dsd.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
            int start = 0;
            while (start < got && std::abs (pcm[(size_t) start * 2]) < 0.05) ++start;
            const int from = start + 35280, len = 88200;
            if (from + len > got) return 0.0;
            return fitSine (pcm.data() + (size_t) from * 2, len, 2, dxd, 1000.0).first;
        };
        const auto doff = play (false, 2.0, -1, dsdCaps, toDsd), don = play (true, 2.0, -1, dsdCaps, toDsd);
        const double relDsd = 20.0 * std::log10 (level (don.dsd, don.rate) / juce::jmax (1e-9, level (doff.dsd, doff.rate)));
        t.check (don.rate > 5000000.0 && std::abs (relDsd + 3.0) < 0.1 && don.allocs == 0,
                 "PCM -> DSD128 : atténuation appliquée en 64 bits avant le modulateur ("_t + juce::String (relDsd, 2) + " dB), 0 allocation");
    }
    // 6. a DSD source is never touched: display only
    {
        auto dsf = t.tempDir.getChildFile ("tp_native.dsf");
        const auto bytes = dsdSine (2822400.0, 0.5, 2, 1000.0, 0.4);
        writeDsf (dsf, bytes, 2, 2822400.0, true);
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; });
        auto st = rig.engine->getSettings();
        st.autoTruePeak = true;
        rig.engine->setSettings (st);
        auto it = EngineRig::item (dsf);
        it.truePeakDb = 3.0;
        rig.engine->setQueue ({ it }, 0, true);
        juce::String node;
        double att = -1.0;
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 8)
            {
                for (auto& nd : rig.engine->getSignalPath().nodes) if (nd.stage == "Headroom") node = nd.detail;
                att = rig.engine->getTruePeakAttenuationDb();
            }
        });
        const auto ref = decodeAllDsd (dsf);
        const auto& got = rig.cap->capturedDsd;
        const bool same = got.size() >= ref.size() && std::equal (ref.begin(), ref.end(), got.begin());
        t.check (same && att == 0.0 && node.contains ("DSD"), "source DSD native (TP +3,0) : flux 1 bit intact, True Peak affiché seulement ("_t + node + ")");
    }
}

NX_TEST_SECTION (125, "Smart Ear Comfort : profil de mastering (DR, aigus 6-10 kHz), E9DS dosé par piste, bit-perfect sur le reste, 0 allocation")
{
    constexpr double pi = juce::MathConstants<double>::pi;
    using nx::comfort::classify;
    using P = nx::comfort::Profile;
    // 1. the profile and the dose
    {
        const auto a = classify (6, -14.0), b = classify (8, -18.0), c = classify (9, -10.0), d = classify (8, -19.0), e = classify (12, -10.0),
                   f = classify (2, 0.0), g = classify (0, -10.0), h = classify (6, std::numeric_limits<double>::quiet_NaN());
        t.check (a.profile == P::Bright && std::abs (a.doseDb - 3.5) < 1e-9 && b.profile == P::Bright && b.doseDb == 1.5 && c.profile == P::Balanced && c.doseDb == 0.0
                     && d.profile == P::Balanced && e.profile == P::Dynamic && e.doseDb == 0.0 && f.doseDb == 6.0 && g.profile == P::Undetermined && g.doseDb == 0.0 && h.profile == P::Unknown,
                 "profil : brillant / compressé (DR <= 8 et aigus >= -18 dB, dose -1,5 à -6 dB), équilibré, dynamique (DR >= 12), indéterminé (sans DR), inconnu"_t);
    }
    // 2. the analysis measures it: a loud steady master with strong 8 kHz, a quiet dynamic recording with bursts at 200 Hz
    auto bright = t.tempDir.getChildFile ("sec_bright.wav"), dyn = t.tempDir.getChildFile ("sec_dynamic.wav");
    const int n = 96000;
    std::vector<int32_t> bPcm ((size_t) n * 2), dPcm ((size_t) n * 2);
    for (int i = 0; i < n; ++i)
    {
        const double x = 0.2 * std::sin (2.0 * pi * 1000.0 * i / 48000.0) + 0.35 * std::sin (2.0 * pi * 8000.0 * i / 48000.0);
        const double y = ((i % 48000) < 2400 ? 0.9 : 0.01) * std::sin (2.0 * pi * 200.0 * i / 48000.0);
        bPcm[(size_t) i * 2] = bPcm[(size_t) i * 2 + 1] = (int32_t) ((uint32_t) std::llround (x * 8388607.0) << 8);
        dPcm[(size_t) i * 2] = dPcm[(size_t) i * 2 + 1] = (int32_t) ((uint32_t) std::llround (y * 8388607.0) << 8);
    }
    writeWav (bright, &bPcm, nullptr, 2, 48000, 24, 24, false);
    writeWav (dyn, &dPcm, nullptr, 2, 48000, 24, 24, false);
    const auto ab = nx::TrackAnalyzer::analyse (bright, 400, [] { return false; });
    const auto ad = nx::TrackAnalyzer::analyse (dyn, 400, [] { return false; });
    const bool measured = ab != nullptr && ad != nullptr;
    t.check (measured && classify (ab->dr, ab->hfShareDb).profile == P::Bright && classify (ad->dr, ad->hfShareDb).profile == P::Dynamic
                 && std::abs (ab->hfShareDb + 3.7) < 1.0 && ad->hfShareDb < -40.0,
             "analyse : master brillant DR"_t + (measured ? juce::String (ab->dr) + ", aigus " + juce::String (ab->hfShareDb, 1) + " dB ; enregistrement dynamique DR"
                                                              + juce::String (ad->dr) + ", aigus " + juce::String (ad->hfShareDb, 1) + " dB" : juce::String ("?")));
    if (! measured) return;
    {
        nx::Library lib;
        juce::String err;
        lib.openInMemory (t.tempDir.getChildFile ("sec_lib"), err);
        nx::TrackInfo tr;
        tr.source = "local"; tr.externalId = tr.uri = "/music/b.flac"; tr.title = "B"; tr.fileSize = 10; tr.modified = 1;
        lib.upsert (tr);
        lib.setProfile (tr.externalId, 6, -12.5);
        lib.upsert (tr);
        const auto kept = lib.getProfile (tr.externalId);
        tr.fileSize = 11;
        lib.upsert (tr);
        t.check (kept.has_value() && kept->first == 6 && kept->second == -12.5 && ! lib.getProfile (tr.externalId).has_value(),
                 "base : colonnes dr / hf_share gardées au rescan, effacées quand le fichier change"_t);
    }

    // 3. engine, PCM to the DAC: the bright master then the dynamic recording, gapless, E9DS in both chains as in the app
    struct Run { std::vector<int32_t> got; std::vector<uint8_t> dsd; double rate = 0; juce::String node, dsp; long allocs = 0; };
    auto play = [&] (bool on, bool known, int liveAt, bool manual, std::function<void (nx::CaptureBackend&)> cfg = {}, std::function<void (nx::EngineSettings&)> more = {})
    {
        EngineRig rig (cfg);
        auto st = rig.engine->getSettings();
        st.smartComfort = on;
        if (more) more (st);
        rig.engine->setSettings (st);
        auto e9 = std::make_shared<nx::E9dsState>();
        if (manual) e9->set (true, 6500.0, 1.0, nx::E9dsState::SplitBand);
        for (const bool dec : { true, false })
        {
            auto chain = std::make_unique<nx::DspChain>();
            chain->processors.push_back (std::make_unique<nx::E9dsProcessor> (e9));
            if (dec) rig.engine->setDecoderDspChain (std::move (chain)); else rig.engine->setDspChain (std::move (chain));
        }
        auto a = EngineRig::item (bright), b = EngineRig::item (dyn);
        if (known) { a.dr = ab->dr; a.hfShareDb = ab->hfShareDb; }
        b.dr = ad->dr; b.hfShareDb = ad->hfShareDb;
        rig.cap->renderFlag = &nxtest_countAllocations;
        nxtest_allocationCount = 0;
        Run r;
        rig.engine->setQueue ({ a, b }, 0, true);
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 20)
                for (auto& nd : rig.engine->getSignalPath().nodes)
                {
                    if (nd.stage == "Confort") r.node = nd.title + " | " + nd.detail;
                    if (nd.stage == "DSP") r.dsp = nd.title;
                }
            if (block == liveAt) rig.engine->setTrackProfile (0, ab->dr, ab->hfShareDb);
        }, 1u << 16);
        rig.cap->renderFlag = nullptr;
        r.allocs = nxtest_allocationCount.load();
        r.got = rig.cap->capturedInt;
        r.dsd = rig.cap->capturedDsd;
        r.rate = rig.cap->config.sampleRate;
        return r;
    };
    auto amp = [] (const std::vector<int32_t>& v, int from, int len, double rate, double freq)
    {
        if ((size_t) (from + len) * 2 > v.size()) return 0.0;
        std::vector<double> x ((size_t) len);
        for (int i = 0; i < len; ++i) x[(size_t) i] = v[(size_t) (from + i) * 2] * (1.0 / 2147483648.0);
        return fitSine (x.data(), len, 1, rate, freq).first;
    };
    auto db = [] (double a, double b) { return 20.0 * std::log10 (juce::jmax (1e-12, a) / juce::jmax (1e-12, b)); };
    auto sameFrom = [&] (const std::vector<int32_t>& got, size_t from, size_t to)
    {
        if (got.size() < to) return false;
        for (size_t i = from; i < to; ++i) if (got[i] != (i < bPcm.size() ? bPcm[i] : dPcm[i - bPcm.size()])) return false;
        return true;
    };
    const size_t end = bPcm.size() + dPcm.size();
    {
        const auto off = play (false, true, -1, false), on = play (true, true, -1, false);
        const double hi = db (amp (on.got, 24000, 48000, 48000.0, 8000.0), 0.35), lo = db (amp (on.got, 24000, 48000, 48000.0, 1000.0), 0.2);
        t.check (sameFrom (off.got, 0, end) && off.node.isEmpty(), "désactivé : les deux pistes arrivent au DAC bit à bit"_t);
        t.check (hi < -0.5 && std::abs (lo) < 0.3 && sameFrom (on.got, bPcm.size() + 14400, end) && on.allocs == 0,
                 "actif : master brillant adouci (8 kHz "_t + juce::String (hi, 2) + " dB, 1 kHz " + juce::String (lo, 2)
                     + " dB), enregistrement dynamique suivant bit à bit (gapless), 0 allocation sur le thread audio");
        t.check (on.node.contains ("Master brillant") && on.node.contains ("E9DS auto") && on.dsp.contains ("Smart Ear Comfort"),
                 "Chemin du signal : "_t + on.node + " ; DSP : " + on.dsp);
        // manual E9DS wins: its own setting, never the automatic one
        const auto man = play (true, true, -1, true);
        t.check (man.dsp.contains ("Split-Band 6,5 kHz") && ! man.dsp.contains ("auto"), "E9DS réglé à la main prioritaire : "_t + man.dsp);
        // the analysis arrives during playback: untouched before, softened after (20 ms ramp)
        const auto live = play (true, false, 12, false);
        const double before = db (amp (live.got, 1000, 8000, 48000.0, 8000.0), 0.35), after = db (amp (live.got, 48000, 40000, 48000.0, 8000.0), 0.35);
        t.check (std::abs (before) < 0.05 && after < -0.5 && live.allocs == 0,
                 "profil arrivé en cours de lecture : 8 kHz "_t + juce::String (before, 2) + " dB avant, " + juce::String (after, 2) + " dB après");
    }
    // 4. PCM -> DSD128: applied in the decoder's chain, before the modulator
    {
        auto dsdCaps = [] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0, 5644800.0, 11289600.0 }; };
        auto toDsd = [] (nx::EngineSettings& st) { st.pcmToDsd = 128; };
        auto level = [&] (const std::vector<uint8_t>& dsd, double dsdRate, double freq)
        {
            constexpr double dxd = 352800.0;
            nx::dsp::DsdToPcm back;
            back.prepare (dsdRate, dxd, 2, 1.0);
            const int bytes = (int) (dsd.size() / 2);
            std::vector<double> pcm ((size_t) (back.maxOutputFor (bytes) + 8) * 2);
            const int got = back.process (dsd.data(), bytes, pcm.data(), back.maxOutputFor (bytes));
            int start = 0;
            while (start < got && std::abs (pcm[(size_t) start * 2]) < 0.05) ++start;
            const int from = start + 70560, len = 176400;
            if (from + len > got) return 0.0;
            return fitSine (pcm.data() + (size_t) from * 2, len, 2, dxd, freq).first;
        };
        const auto off = play (false, true, -1, false, dsdCaps, toDsd), on = play (true, true, -1, false, dsdCaps, toDsd);
        const double hi = db (level (on.dsd, on.rate, 8000.0), level (off.dsd, off.rate, 8000.0)), lo = db (level (on.dsd, on.rate, 1000.0), level (off.dsd, off.rate, 1000.0));
        t.check (on.rate > 5000000.0 && hi < -0.5 && std::abs (lo) < 0.3 && on.allocs == 0,
                 "PCM -> DSD128 : adouci en DXD avant le modulateur (8 kHz "_t + juce::String (hi, 2) + " dB, 1 kHz " + juce::String (lo, 2) + " dB), 0 allocation");
    }
    // 5. a native DSD source is never touched
    {
        auto dsf = t.tempDir.getChildFile ("sec_native.dsf");
        writeDsf (dsf, dsdSine (2822400.0, 0.5, 2, 8000.0, 0.4), 2, 2822400.0, true);
        EngineRig rig ([] (nx::CaptureBackend& c) { c.capabilities.nativeDsdRates = { 2822400.0 }; });
        auto st = rig.engine->getSettings();
        st.smartComfort = true;
        rig.engine->setSettings (st);
        auto e9 = std::make_shared<nx::E9dsState>();
        for (const bool dec : { true, false })
        {
            auto chain = std::make_unique<nx::DspChain>();
            chain->processors.push_back (std::make_unique<nx::E9dsProcessor> (e9));
            if (dec) rig.engine->setDecoderDspChain (std::move (chain)); else rig.engine->setDspChain (std::move (chain));
        }
        auto it = EngineRig::item (dsf);
        it.dr = 4; it.hfShareDb = -3.0;
        rig.engine->setQueue ({ it }, 0, true);
        juce::String node;
        double dose = -1.0;
        rig.runToEnd (1024, 60.0, [&] (int block)
        {
            if (block == 8)
            {
                for (auto& nd : rig.engine->getSignalPath().nodes) if (nd.stage == "Confort") node = nd.detail;
                dose = rig.engine->getComfortDoseDb();
            }
        });
        const auto ref = decodeAllDsd (dsf);
        const auto& got = rig.cap->capturedDsd;
        t.check (got.size() >= ref.size() && std::equal (ref.begin(), ref.end(), got.begin()) && dose == 0.0 && node.contains ("DSD"),
                 "source DSD native (profil brillant) : flux 1 bit intact, profil affiché seulement ("_t + node + ")");
    }
}
