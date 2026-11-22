// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "PlaybackEngine.h"
#include <algorithm>
#include "../Audio/SacdIso.h"
#include "../Core/AudioLog.h"
#include "../Audio/MemoryPlayStream.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include "../DSP/DsdToPcm.h"
#include "../DSP/Resampler.h"
#include "../DSP/PcmToDsd.h"
#include "../Audio/LoopingDecoder.h"
#include <cmath>
#include <cstring>

namespace nx
{

namespace
{
    constexpr size_t kRingBytes = 8u << 20;      // 8 Mo: ~2,7 s of stereo DXD 384 kHz, ~3 s of DSD512
    constexpr int kChunkFrames = 8192;
    constexpr int kDopMinPrerollMs = 400;        // true DSD silence (0x05/0xFA markers + 0x69) before the first music
    constexpr int kDopStableCallbacks = 30;      // consecutive stable hardware buffers before the timeline starts
    constexpr uint8_t kDsdIdle = 0x69;           // DSD silence pattern

    inline int32_t doubleToInt32 (double x) noexcept
    {
        double v = std::floor (x * 2147483648.0 + 0.5);
        if (v > 2147483647.0) v = 2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        return (int32_t) (int64_t) v;
    }

    inline int popcount8 (uint8_t b) noexcept
    {
        b = (uint8_t) (b - ((b >> 1) & 0x55));
        b = (uint8_t) ((b & 0x33) + ((b >> 2) & 0x33));
        return (b + (b >> 4)) & 0x0F;
    }
}

PlaybackEngine::PlaybackEngine (std::unique_ptr<OutputBackend> b)
    : juce::Thread ("NX decoder"), backend (std::move (b)), ring (kRingBytes)
{
    planar.assign ((size_t) 8 * maxBlock, 0.0);
    decPlanar.assign ((size_t) 8 * 16384, 0.0);
    for (int c = 0; c < 8; ++c) planarPtrs[(size_t) c] = planar.data() + (size_t) c * maxBlock;
    rtBytes.assign ((size_t) maxBlock * 2 * 8, 0);
    rtTap.assign ((size_t) maxBlock * 2 * 8, 0.0f);
    tmpInt.resize ((size_t) kChunkFrames * 8 * 2);
    tmpDouble.resize ((size_t) kChunkFrames * 8 * 8);
    tmpDouble2.resize ((size_t) kChunkFrames * 8 * 8);
    tmpBytes.resize ((size_t) kChunkFrames * 8 * 16);
    for (auto& r : requantizers) r.setBits (24);

    if (backend != nullptr && backend->getDeviceNames().size() > 0)
        setDevice (backend->getDeviceNames()[0]);

    startThread (juce::Thread::Priority::high);
    startTimerHz (60);
}

PlaybackEngine::~PlaybackEngine()
{
    stopTimer();
    signalThreadShouldExit();
    stopThread (4000);
    if (deviceRunning) backend->stop();
    backend->close();
    if (auto* p = pendingChain.exchange (nullptr)) juce::ignoreUnused (p);
}

//==============================================================================
void PlaybackEngine::setBackend (std::unique_ptr<OutputBackend> newBackend)
{
    stop();
    if (deviceRunning) backend->stop();
    backend->close();
    deviceRunning = false;
    backend = std::move (newBackend);
    activePlanId = 0;
    if (backend->getDeviceNames().size() > 0)
        setDevice (backend->getDeviceNames()[0]);
}

void PlaybackEngine::setDevice (const juce::String& name)
{
    const bool wasPlaying = state.load() == State::Playing || state.load() == State::Paused;
    const int index = currentIndex.load();
    const double pos = getPositionSeconds();
    if (deviceRunning) { backend->stop(); deviceRunning = false; }
    backend->close();
    activePlanId = 0;

    auto newCaps = backend->probe (name);
    {
        std::lock_guard<std::mutex> l (controlLock);
        deviceName = name;
        sharedFallback = false;
        caps = newCaps;
    }
    if (wasPlaying && index >= 0)
        requestFlushStart (index, pos);   // replan for the new DAC
}

DeviceCapabilities PlaybackEngine::getCapabilities() const
{
    std::lock_guard<std::mutex> l (controlLock);
    return caps;
}

void PlaybackEngine::setSettings (const EngineSettings& s, bool replanCurrent)
{
    {
        std::lock_guard<std::mutex> l (controlLock);
        settings = s;
    }
    fadesOn = s.transitionFades;
    if (replanCurrent && state.load() != State::Stopped && currentIndex.load() >= 0)
        requestFlushStart (currentIndex.load(), getPositionSeconds());
}

EngineSettings PlaybackEngine::getSettings() const
{
    std::lock_guard<std::mutex> l (controlLock);
    return settings;
}

void PlaybackEngine::setLoop (double a, double b)
{
    if (b - a < 0.05 || currentIndex.load() < 0) return;
    loopA = juce::jmax (0.0, a);
    loopB = b;
    loopIndex = currentIndex.load();
    double pos = getPositionSeconds();
    if (pos < a || pos >= b) pos = a;
    // The ring already holds what follows the play position: re-read once, from here, through the loop.
    if (state.load() != State::Stopped) requestFlushStart (currentIndex.load(), pos);
}

void PlaybackEngine::clearLoop (bool rebuffer)
{
    const bool wasActive = loopIndex.load() >= 0;
    loopIndex = -1;
    loopA = -1.0;
    loopB = -1.0;
    if (rebuffer && wasActive && state.load() != State::Stopped && currentIndex.load() >= 0)
        requestFlushStart (currentIndex.load(), getPositionSeconds());
}

EngineSettings PlaybackEngine::getEffectiveSettings() const
{
    std::lock_guard<std::mutex> l (controlLock);
    return resolveProfile (settings, deviceName);
}

juce::String PlaybackEngine::getLastError() const
{
    std::lock_guard<std::mutex> l (controlLock);
    return lastError;
}

//==============================================================================
void PlaybackEngine::setQueue (std::vector<PlayableItem> items, int startIndex, bool startPlaying)
{
    queue = std::move (items);
    const int first = queue.empty() ? 0 : juce::jlimit (0, (int) queue.size() - 1, startIndex);
    {
        std::lock_guard<std::mutex> l (controlLock);
        decoderQueue = queue;
        rebuildOrderLocked (first);
    }
    if (queue.empty()) { stop(); return; }
    startIndex = first;
    if (startPlaying) playIndex (startIndex);
    else currentIndex = startIndex;
}

void PlaybackEngine::appendToQueue (std::vector<PlayableItem> items)
{
    const int firstNew = (int) queue.size();
    for (auto& i : items) queue.push_back (std::move (i));
    std::lock_guard<std::mutex> l (controlLock);
    decoderQueue = queue;
    // the new tracks join the end of the shuffled pass, shuffled among themselves
    const auto from = playOrder.size();
    for (int k = firstNew; k < (int) queue.size(); ++k) playOrder.push_back (k);
    if (playOrder.size() > from) std::shuffle (playOrder.begin() + (std::ptrdiff_t) from, playOrder.end(), orderRng);
}

void PlaybackEngine::playIndex (int index, double startSeconds)
{
    if (index < 0 || index >= (int) queue.size()) return;
    state = State::Playing;
    requestFlushStart (index, startSeconds);
    if (onStateChanged) onStateChanged();
}

void PlaybackEngine::play()
{
    if (state.load() == State::Paused) { state = State::Playing; if (onStateChanged) onStateChanged(); return; }
    if (state.load() == State::Stopped && ! queue.empty())
        playIndex (juce::jmax (0, currentIndex.load()), 0.0);
}

void PlaybackEngine::pause()
{
    if (state.load() == State::Playing) { state = State::Paused; if (onStateChanged) onStateChanged(); }
}

void PlaybackEngine::togglePlayPause()
{
    if (state.load() == State::Playing) pause(); else play();
}

void PlaybackEngine::stop()
{
    state = State::Stopped;
    // let the audio thread fade the music out (10 ms) before the ring is dropped and the device closed
    if (deviceRunning && backend->isRealtime())
        for (const auto t0 = juce::Time::getMillisecondCounter(); ! xfSilent.load() && juce::Time::getMillisecondCounter() - t0 < 40;)
            juce::Thread::sleep (1);
    {
        std::lock_guard<std::mutex> l (controlLock);
        flushCommand = {};
    }
    stopDecoding = true;
    flushGen.fetch_add (1);                      // the audio thread drops everything
    reconfigurePlanId = 0;
    positionUnits = 0;
    if (keepsDopCarrier())
    {
        // DoP carrier keep-alive: the device stays open and keeps sending 0x05/0xFA + 0x69 silence, so the
        // DAC never loses its DSD lock between tracks or after a stop (released on device / backend change).
        if (onStateChanged) onStateChanged();
        return;
    }
    if (deviceRunning) { backend->stop(); deviceRunning = false; }
    backend->close();
    activePlanId = 0;
    if (onStateChanged) onStateChanged();
}

void PlaybackEngine::rebuildOrderLocked (int first)
{
    playOrder.clear();
    const int n = (int) decoderQueue.size();
    for (int k = 0; k < n; ++k) if (k != first) playOrder.push_back (k);
    std::shuffle (playOrder.begin(), playOrder.end(), orderRng);
    if (first >= 0 && first < n) playOrder.insert (playOrder.begin(), first);
}

int PlaybackEngine::nextLocked (int index, bool userAction)
{
    const int n = (int) decoderQueue.size();
    if (n == 0) return -1;
    if (repeatMode == Repeat::One && ! userAction) return index;
    if (! shuffleOn) return index + 1 < n ? index + 1 : (repeatMode != Repeat::Off ? 0 : -1);
    const auto it = std::find (playOrder.begin(), playOrder.end(), index);
    if (it != playOrder.end() && it + 1 != playOrder.end()) return *(it + 1);
    if (repeatMode == Repeat::Off) return -1;
    rebuildOrderLocked (index);                  // a new pass starting from the track just played: it is not repeated back to back
    return n > 1 ? playOrder[1] : playOrder[0];
}

void PlaybackEngine::setShuffle (bool on)
{
    std::lock_guard<std::mutex> l (controlLock);
    shuffleOn = on;
    if (on) rebuildOrderLocked (currentIndex.load());
}
bool PlaybackEngine::isShuffle() const { std::lock_guard<std::mutex> l (controlLock); return shuffleOn; }
void PlaybackEngine::setRepeat (Repeat mode) { std::lock_guard<std::mutex> l (controlLock); repeatMode = mode; }
PlaybackEngine::Repeat PlaybackEngine::getRepeat() const { std::lock_guard<std::mutex> l (controlLock); return repeatMode; }

int PlaybackEngine::getRemainingCount() const
{
    std::lock_guard<std::mutex> l (controlLock);
    const int cur = currentIndex.load(), n = (int) decoderQueue.size();
    if (! shuffleOn) return juce::jmax (0, n - 1 - cur);
    const auto it = std::find (playOrder.begin(), playOrder.end(), cur);
    return it == playOrder.end() ? 0 : (int) (playOrder.end() - it) - 1;
}

int PlaybackEngine::getNextIndex (int index, bool userAction) const
{
    std::lock_guard<std::mutex> l (controlLock);
    return const_cast<PlaybackEngine*> (this)->nextLocked (index, userAction);
}

void PlaybackEngine::next()
{
    const int i = getNextIndex (currentIndex.load(), true);
    if (i >= 0) playIndex (i);
}

void PlaybackEngine::previous()
{
    const int i = currentIndex.load();
    int prev = -1;
    {
        std::lock_guard<std::mutex> l (controlLock);
        if (shuffleOn)
        {
            const auto it = std::find (playOrder.begin(), playOrder.end(), i);
            if (it != playOrder.end() && it != playOrder.begin()) prev = *(it - 1);
        }
        else prev = i - 1;
    }
    if (getPositionSeconds() > 3.0 || prev < 0) seek (0.0);
    else playIndex (prev);
}

void PlaybackEngine::seek (double seconds)
{
    const int i = currentIndex.load();
    if (i < 0 || i >= (int) queue.size()) return;
    if (state.load() == State::Stopped) state = State::Paused;
    requestFlushStart (i, juce::jmax (0.0, seconds));
}

void PlaybackEngine::requestFlushStart (int index, double seconds)
{
    const auto gen = flushGen.fetch_add (1) + 1;
    {
        std::lock_guard<std::mutex> l (controlLock);
        flushCommand = { true, index, seconds, gen };
    }
    stopDecoding = false;
    currentIndex = index;
    positionBase = seconds;
    positionUnits = 0;
}

double PlaybackEngine::getPositionSeconds() const noexcept
{
    const double ups = positionUnitsPerSecond.load();
    return positionBase.load() + (ups > 0 ? (double) positionUnits.load() / ups : 0.0);
}

double PlaybackEngine::getDurationSeconds() const
{
    const int i = currentIndex.load();
    if (i >= 0 && i < (int) queue.size()) return queue[(size_t) i].durationHint;
    return 0.0;
}

void PlaybackEngine::setVolume (double gain)
{
    volume = juce::jlimit (0.0, 1.0, gain);
    pathChangedFlag = true;
}

void PlaybackEngine::setFixedVolume (bool fixed)
{
    fixedVolume = fixed;
    pathChangedFlag = true;
}

void PlaybackEngine::setDspChain (std::unique_ptr<DspChain> chain)
{
    if (chain == nullptr) chain = std::make_unique<DspChain>();
    const auto status = backend->getStatus();
    if (status.open)
        chain->prepare (status.sampleRate, juce::jmax (1, getCurrentPlan().channels), maxBlock);
    auto* raw = chain.get();
    chainStore.push_back (std::move (chain));
    if (deviceRunning)
        pendingChain.store (raw);
    else
        activeChain = raw;
    pathChangedFlag = true;
}

void PlaybackEngine::setDecoderDspChain (std::unique_ptr<DspChain> chain)
{
    if (chain == nullptr) chain = std::make_unique<DspChain>();
    auto* raw = chain.get();
    chainStore.push_back (std::move (chain));
    if (auto* never = pendingDecChain.exchange (raw))            // not picked up yet: no thread has it, drop it here
        for (auto it = chainStore.begin(); it != chainStore.end(); ++it)
            if (it->get() == never) { chainStore.erase (it); break; }
}

PlaybackPlan PlaybackEngine::getCurrentPlan() const
{
    std::lock_guard<std::mutex> l (controlLock);
    auto it = plans.find (playingPlanId.load());
    if (it == plans.end()) it = plans.find (activePlanId.load());
    return it != plans.end() ? it->second : PlaybackPlan();
}

//==============================================================================
// Decoder thread
//==============================================================================
void PlaybackEngine::run()
{
    while (! threadShouldExit())
    {
        FlushCommand cmd;
        {
            std::lock_guard<std::mutex> l (controlLock);
            cmd = flushCommand;
            flushCommand.pending = false;
        }

        if (cmd.pending)
        {
            decoderGen = cmd.gen;
            decoder.reset();
            int index = cmd.index;
            bool opened = false;
            while (! opened && ! threadShouldExit())
            {
                size_t n;
                { std::lock_guard<std::mutex> l (controlLock); n = decoderQueue.size(); }
                if (index >= (int) n) break;
                opened = openItem (index, cmd.seconds, true);
                if (! opened) { ++index; cmd.seconds = 0.0; }
            }
            if (! opened)
            {
                const RingEvent end { RingEvent::End, bytesWritten, decoderGen, index, 0, 0.0 };
                events.write (&end, 1);
            }
        }

        if (stopDecoding.load())
            decoder.reset();
        decoderBusy = decoder != nullptr;

        if (decoder == nullptr || flushGen.load() != decoderGen)
        {
            wait (5);
            continue;
        }

        auto writable = (int) std::min<size_t> (ring.writable(), 1u << 20);
        if (decHavePlan && decPlan.remodulates() && decoderDspOn())
        {
            // live DSP before the modulator: keep 0.5 s ahead only, so a change of EQ is heard at once
            // (0.9 s while a cold DoP start waits for its 0.8 s of pre-roll)
            const double seconds = coldPreroll.load (std::memory_order_relaxed) ? 0.9 : 0.5;
            const auto lead = (size_t) (decPlan.ringUnitsPerSecond() * seconds) * (size_t) decPlan.ringFrameBytes();
            writable = ring.readable() >= lead ? 0 : (int) std::min<size_t> ((size_t) writable, lead - ring.readable());
        }
        if (writable < 16384)
        {
            wait (3);
            continue;
        }

        if (loopDecoder != nullptr && loopDecoder->isAtWrap())
        {
            // The next byte written is the start of the loop again: mark it exactly.
            const auto f = loopDecoder->getFormat();
            const double unitRate = f.isDsd() ? f.sampleRate / 8.0 : f.sampleRate;
            const RingEvent pe { RingEvent::Position, bytesWritten, decoderGen, decIndex, decPlanId, (double) loopDecoder->getLoopA() / unitRate };
            events.write (&pe, 1);
            loopWrapsSeen = loopDecoder->getWrapCount() + 1;
        }
        const int produced = produce (writable);
        if (loopDecoder != nullptr && loopDecoder->getWrapCount() > loopWrapsSeen)   // wrapped without notice (B past the end)
        {
            // The loop wrapped inside this chunk: tell the audio thread where the clock restarts.
            loopWrapsSeen = loopDecoder->getWrapCount();
            const auto f = loopDecoder->getFormat();
            const double unitRate = f.isDsd() ? f.sampleRate / 8.0 : f.sampleRate;
            const RingEvent pe { RingEvent::Position, bytesWritten, decoderGen, decIndex, decPlanId, (double) loopDecoder->getPosition() / unitRate };
            events.write (&pe, 1);
        }
        if (produced > 0)
            continue;

        // End of this track: gapless move to the next one.
        bool moved = false;
        int index = decIndex + 1;
        size_t n;
        { std::lock_guard<std::mutex> l (controlLock); n = decoderQueue.size(); }
        // play order: shuffle, repeat the track / the queue; a track that cannot be opened is skipped (each one tried once at most)
        for (size_t tries = 0, cand = (size_t) decIndex; ! moved && tries <= n && ! threadShouldExit() && flushGen.load() == decoderGen; ++tries)
        {
            { std::lock_guard<std::mutex> l (controlLock); index = nextLocked ((int) cand, tries > 0); }
            if (index < 0) break;
            moved = openItem (index, 0.0, false);
            cand = (size_t) index;
        }
        if (! moved) index = decIndex + 1;

        if (! moved && flushGen.load() == decoderGen)
        {
            drainConverters();
            if (decPlan.wire != WireFormat::Pcm && (bytesWritten / juce::jmax (1, decPlan.ringFrameBytes())) % 2 != 0)
            {
                std::array<uint8_t, 8> idle; idle.fill (kDsdIdle);   // keep DoP words whole
                writeAll (idle.data(), (size_t) decPlan.ringFrameBytes());
            }
            const RingEvent end { RingEvent::End, bytesWritten, decoderGen, index, 0, 0.0 };
            events.write (&end, 1);
            decoder.reset();
            decHavePlan = false;
        }
    }
}

bool PlaybackEngine::openItem (int index, double startSeconds, bool flush)
{
    PlayableItem item;
    DeviceCapabilities c;
    EngineSettings s;
    {
        std::lock_guard<std::mutex> l (controlLock);
        if (index < 0 || index >= (int) decoderQueue.size()) return false;
        item = decoderQueue[(size_t) index];
        c = caps;
        s = resolveProfile (settings, deviceName);
    }

    std::unique_ptr<juce::InputStream> stream;
    if (item.openStream)          stream = item.openStream();
    else if (streamResolver)      stream = streamResolver (item);
    bool inMemory = false;
    if (stream == nullptr && sacd::isVirtualUri (item.uri))
    {
        // a SACD image track: read from the disc image (a whole .iso is far too big for Memory Play)
        auto f = std::make_unique<juce::FileInputStream> (sacd::realFile (item.uri));
        if (f->openedOk()) stream = std::make_unique<juce::BufferedInputStream> (f.release(), 1 << 17, true);
    }
    if (stream == nullptr && juce::File::isAbsolutePath (item.uri) && ! sacd::isVirtualUri (item.uri))
    {
        // Memory Play: the file is copied into RAM (in the background, playback starts at once).
        if (s.memoryPlay)
            if (auto m = MemoryPlayStream::open (juce::File (item.uri), (juce::int64) juce::jmax (1, s.memoryPlayMaxMb) * 1024 * 1024))
            {
                stream = std::move (m);
                inMemory = true;
            }
        if (stream == nullptr)
        {
            auto f = std::make_unique<juce::FileInputStream> (juce::File (item.uri));
            if (f->openedOk()) stream = std::make_unique<juce::BufferedInputStream> (f.release(), 1 << 17, true);
        }
    }

    juce::String err;
    auto dec = stream != nullptr ? openDecoder (std::move (stream), item.uri, err) : nullptr;
    if (dec == nullptr)
    {
        std::lock_guard<std::mutex> l (controlLock);
        lastError = (item.title.isNotEmpty() ? item.title : item.uri) + " : " + (err.isNotEmpty() ? err : juce::String ("illisible"));
        errorFlag = true;
        return false;
    }

    if (s.freeEdition)
    {
        const auto f = dec->getFormat();
        if (f.isDsd() || f.sampleRate > 192000.5 || sacd::isVirtualUri (item.uri))
        {
            // Aura Free plays PCM up to 24 bits / 192 kHz: this track is skipped, the app offers Premium
            std::lock_guard<std::mutex> l (controlLock);
            lastError = juce::String (studioMasterError) + (item.title.isNotEmpty() ? item.title : item.uri);
            errorFlag = true;
            return false;
        }
    }
    // A SACD's multichannel track that would be mixed down: the disc's own stereo area (the engineer's mix) instead
    bool stereoLayer = false;
    if (sacd::isVirtualUri (item.uri) && dec->getFormat().numChannels > 2 && makePlan (dec->getFormat(), c, s).downmix)
    {
        juce::File iso;
        int area = 0, track = 0;
        if (sacd::parseVirtualUri (item.uri, iso, area, track) && area == 1)
        {
            auto f = std::make_unique<juce::FileInputStream> (iso);
            juce::String err2;
            if (f->openedOk())
                if (auto two = sacd::openTrack (std::make_unique<juce::BufferedInputStream> (f.release(), 1 << 17, true), 0, track, err2);
                    two != nullptr && two->getFormat().numChannels == 2)
                {
                    dec = std::move (two);
                    stereoLayer = true;
                }
        }
    }

    auto plan = makePlan (dec->getFormat(), c, s);
    if (s.channelMode != ChannelMode::Multichannel)
        plan.channels = juce::jmin (plan.channels, juce::jlimit (1, 8, s.maxChannels));
    if (stereoLayer)
    {
        plan.stereoLayer = true;
        plan.reason += " · "_t + "couche stéréo du SACD (au lieu du multicanal)"_t;
    }
    plan.inMemory = inMemory;
    plan.fileBytes = sacd::isVirtualUri (item.uri) ? 0 : juce::File::isAbsolutePath (item.uri) ? juce::File (item.uri).getSize() : 0;

    const bool sameWire = decHavePlan && plan.sameWire (decPlan);
    if (! flush && ! sameWire) drainConverters();

    const bool keepState = ! flush && sameWire && decPlan.conversion == plan.conversion
                           && decPlan.source.sampleRate == plan.source.sampleRate
                           && decPlan.conversionRate == plan.conversionRate;
    preparePlanConverters (plan, keepState);

    if (startSeconds > 0.0)
    {
        const auto f = dec->getFormat();
        const auto pos = (juce::int64) (startSeconds * (f.isDsd() ? f.sampleRate / 8.0 : f.sampleRate));
        dec->seek (pos);
    }

    const int planId = nextPlanId.fetch_add (1);
    PlanWire w;
    w.wire = plan.wire;
    w.rate = plan.deviceRate;
    w.channels = plan.channels;
    w.ringBits = plan.ringBitsHint;
    w.needsRequantize = plan.needsRequantize;
    w.unitsPerSecond = plan.ringUnitsPerSecond();
    w.valid = true;
    planWires[(size_t) (planId & 63)] = w;
    {
        std::lock_guard<std::mutex> l (controlLock);
        plans[planId] = plan;
        while (plans.size() > 32) plans.erase (plans.begin());
    }

    const RingEvent e { RingEvent::Track, bytesWritten, decoderGen, index, planId, startSeconds };
    events.write (&e, 1);

    // A-B loop on this track: wrap the decoder (positions in its units, the start is moved inside the loop).
    loopDecoder = nullptr;
    loopWrapsSeen = 0;
    if (loopIndex.load() == index && loopB.load() > loopA.load())
    {
        const auto f = dec->getFormat();
        const double unitRate = f.isDsd() ? f.sampleRate / 8.0 : f.sampleRate;
        auto looping = std::make_unique<LoopingDecoder> (std::move (dec), (juce::int64) (loopA.load() * unitRate), (juce::int64) (loopB.load() * unitRate),
                                                         (int) (loopCrossfadeMs * 0.001 * unitRate));
        const auto pos = (juce::int64) (startSeconds * unitRate);
        if (pos < looping->getLoopA() || startSeconds >= loopB.load()) looping->seek (looping->getLoopA());
        loopDecoder = looping.get();
        dec = std::move (looping);
    }
    decoder = std::move (dec);
    decIndex = index;
    decPlan = plan;
    decPlanId = planId;
    decHavePlan = true;

    if (flush && ! planWires[(size_t) (activePlanId.load() & 63)].same (w))
        reconfigurePlanId = planId;
    return true;
}

void PlaybackEngine::preparePlanConverters (const PlaybackPlan& plan, bool keepState)
{
    if (keepState) return;
    // a DSD output cannot be faded after the modulator: fresh starts fade in in DXD, before it
    remodFadeIn = getEffectiveSettings().transitionFades ? (int) (transitionFadeMs * 0.001 * 352800.0) : 0;
    dsdConverter.reset();
    resampler.reset();
    pcmToDsd.reset();
    const int ch = plan.source.numChannels;
    const auto eff = getEffectiveSettings();
    const bool minPhase = plan.filterShape == FilterShape::MinimumPhase;
    // Minimum phase ("Analog Warmth"): a gentler slope that starts below 20 kHz, fully attenuated at Nyquist.
    const double passFraction = minPhase ? 0.86 : 0.907;

    if (plan.conversion == PlaybackPlan::Conversion::DsdToPcm)
    {
        dsdConverter = std::make_unique<dsp::DsdToPcm>();
        dsdConverter->prepare (plan.source.sampleRate, plan.conversionRate, ch,
                               std::pow (10.0, eff.dsdToPcmGainDb / 20.0), minPhase);
        if (std::abs (dsdConverter->getOutputRate() - plan.deviceRate) > 0.5)
        {
            resampler = std::make_unique<dsp::Resampler>();
            resampler->prepare (dsdConverter->getOutputRate(), plan.deviceRate, ch, 160.0, passFraction, minPhase);
        }
    }
    else if (plan.conversion == PlaybackPlan::Conversion::Resample)
    {
        resampler = std::make_unique<dsp::Resampler>();
        resampler->prepare (plan.source.sampleRate, plan.deviceRate, ch, 160.0, passFraction, minPhase);
    }
    else if (plan.conversion == PlaybackPlan::Conversion::PcmToDsd)
    {
        // source -> DXD 352,8 kHz (the DSD rates are multiples of 44,1 kHz; a 48 kHz family source is converted too),
        // then the modulator (its own x8 / x16 / x32 interpolation)
        constexpr double dxd = 352800.0;
        if (plan.source.sampleRate != dxd)
        {
            resampler = std::make_unique<dsp::Resampler>();
            resampler->prepare (plan.source.sampleRate, dxd, ch, 160.0, passFraction, minPhase);
        }
        pcmToDsd = std::make_unique<dsp::PcmToDsd>();
        pcmToDsd->prepare (dxd, plan.conversionRate, plan.downmix ? 2 : ch, eff.sdmOrder, eff.pcmToDsdHeadroomDb);
    }
    else if (plan.conversion == PlaybackPlan::Conversion::DsdRemodulate)
    {
        // DSD -> DXD with gain 2: SACD 0 dB becomes 1.0, the scale the modulator expects from PCM (it halves it back)
        dsdConverter = std::make_unique<dsp::DsdToPcm>();
        dsdConverter->prepare (plan.source.sampleRate, 352800.0, ch, 2.0, false);
        pcmToDsd = std::make_unique<dsp::PcmToDsd>();
        pcmToDsd->prepare (352800.0, plan.conversionRate, plan.downmix ? 2 : ch, eff.sdmOrder, eff.pcmToDsdHeadroomDb);
    }

    // Only converted streams are shaped: a bit-perfect plan never goes through this stage.
    const bool converted = plan.conversion == PlaybackPlan::Conversion::DsdToPcm || plan.conversion == PlaybackPlan::Conversion::Resample;
    shaper.prepare (plan.deviceRate, ch, converted && eff.warmth,
                    plan.conversion == PlaybackPlan::Conversion::DsdToPcm && eff.limiter && eff.dsdToPcmGainDb > 0.0);
}

void PlaybackEngine::writeAll (const void* data, size_t numBytes)
{
    auto* p = static_cast<const uint8_t*> (data);
    while (numBytes > 0 && ! threadShouldExit() && flushGen.load() == decoderGen)
    {
        const auto n = ring.write (p, numBytes);
        p += n;
        numBytes -= n;
        bytesWritten += (juce::int64) n;
        if (numBytes > 0) wait (2);
    }
}

/** Device output o <- source channel route[o] (silence for -1): mono doubled, speaker map of a multichannel output. */
template <typename T>
static void routeChannels (const T* in, T* out, int frames, int srcCh, int outCh, const std::array<int, 8>& route, T silence) noexcept
{
    for (int i = 0; i < frames; ++i)
        for (int o = 0; o < outCh; ++o)
        {
            const int c = route[(size_t) o];
            out[(size_t) i * (size_t) outCh + (size_t) o] = c >= 0 && c < srcCh ? in[(size_t) i * (size_t) srcCh + (size_t) c] : silence;
        }
}

/** Multichannel -> stereo, in place over `frames` interleaved frames of srcCh channels (out: 2 channels).
    6 ch = FL FR C LFE SL SR, 5 ch = FL FR C SL SR, 4 ch = FL FR SL SR, 3 ch = FL FR C. LFE is left out;
    L = FL + 0.707 C + 0.707 SL, R = FR + 0.707 C + 0.707 SR, scaled by 1 / (1 + 0.707 + 0.707): no clipping possible. */
void PlaybackEngine::foldToStereo (double* pcm, int frames, int srcCh) noexcept
{
    constexpr double k = 0.70710678118654752;
    const bool hasC = srcCh == 3 || srcCh == 5 || srcCh == 6;
    const int sl = srcCh == 6 ? 4 : srcCh == 5 ? 3 : srcCh == 4 ? 2 : -1;
    const double g = 1.0 / (1.0 + (hasC ? k : 0.0) + (sl >= 0 ? k : 0.0));
    for (int i = 0; i < frames; ++i)
    {
        const double* in = pcm + (size_t) i * (size_t) srcCh;
        const double c = hasC ? k * in[2] : 0.0;
        const double l = in[0] + c + (sl >= 0 ? k * in[sl] : 0.0);
        const double r = in[1] + c + (sl >= 0 ? k * in[sl + 1] : 0.0);
        pcm[(size_t) i * 2] = g * l;
        pcm[(size_t) i * 2 + 1] = g * r;
    }
}

int PlaybackEngine::produce (int writableBytes)
{
    const auto& plan = decPlan;
    const auto src = decoder->getFormat();
    const int srcCh = src.numChannels;
    const int outCh = plan.channels;
    const int frameBytes = plan.ringFrameBytes();
    const int maxOutFrames = juce::jmin (kChunkFrames, writableBytes / frameBytes - 4);
    if (maxOutFrames <= 0) return 1;

    auto doublesToRing = [&] (double* in, int frames)
    {
        auto* out = tmpInt.data();
        if (plan.downmix)
        {
            foldToStereo (in, frames, srcCh);
            for (int i = 0; i < frames * 2; ++i) out[(size_t) i] = doubleToInt32 (in[(size_t) i]);
            writeAll (out, (size_t) frames * 2 * 4);
            return;
        }
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < srcCh; ++c)
                out[(size_t) i * srcCh + c] = doubleToInt32 (in[(size_t) i * srcCh + c]);
        if (plan.identityRoute()) { writeAll (out, (size_t) frames * (size_t) outCh * 4); return; }
        auto* exp = reinterpret_cast<int32_t*> (tmpBytes.data());
        routeChannels (out, exp, frames, srcCh, outCh, plan.route, (int32_t) 0);
        writeAll (exp, (size_t) frames * (size_t) outCh * 4);
    };

    auto expandAndWriteInt = [&] (const int32_t* in, int frames)
    {
        if (plan.downmix)
        {
            for (int i = 0; i < frames * srcCh; ++i) tmpDouble[(size_t) i] = in[(size_t) i] * (1.0 / 2147483648.0);
            doublesToRing (tmpDouble.data(), frames);
            return;
        }
        if (plan.identityRoute()) { writeAll (in, (size_t) frames * (size_t) outCh * 4); return; }
        auto* out = reinterpret_cast<int32_t*> (tmpBytes.data());
        routeChannels (in, out, frames, srcCh, outCh, plan.route, (int32_t) 0);
        writeAll (out, (size_t) frames * (size_t) outCh * 4);
    };

    if (plan.remodulates()) return produceRemodulated (maxOutFrames);

    if (plan.wire != WireFormat::Pcm)
    {
        const int n = decoder->readDsd (tmpBytes.data() + (size_t) kChunkFrames * 8, maxOutFrames);
        if (n <= 0) return 0;
        const uint8_t* in = tmpBytes.data() + (size_t) kChunkFrames * 8;
        if (plan.identityRoute()) writeAll (in, (size_t) n * (size_t) outCh);
        else
        {
            routeChannels (in, tmpBytes.data(), n, srcCh, outCh, plan.route, (uint8_t) kDsdIdle);   // unused outputs: DSD silence
            writeAll (tmpBytes.data(), (size_t) n * (size_t) outCh);
        }
        return n;
    }

    switch (plan.conversion)
    {
        case PlaybackPlan::Conversion::PcmToDsd:     // handled above (a DSD wire)
        case PlaybackPlan::Conversion::DsdRemodulate:
            return produceRemodulated (maxOutFrames);

        case PlaybackPlan::Conversion::None:
        {
            const int n = decoder->readInt (tmpInt.data(), maxOutFrames);
            if (n > 0) expandAndWriteInt (tmpInt.data(), n);
            return n;
        }

        case PlaybackPlan::Conversion::FloatToInt:
        {
            const int n = decoder->readDouble (tmpDouble.data(), maxOutFrames);
            if (n > 0) doublesToRing (tmpDouble.data(), n);
            return n;
        }

        case PlaybackPlan::Conversion::Resample:
        {
            const double ratio = resampler->getRatio();
            const int inFrames = juce::jmax (1, (int) std::floor ((maxOutFrames - 4) / ratio));
            int n = 0;
            if (src.kind == SampleKind::PcmInt)
            {
                n = decoder->readInt (tmpInt.data(), juce::jmin (inFrames, kChunkFrames));
                for (int i = 0; i < n * srcCh; ++i) tmpDouble[(size_t) i] = tmpInt[(size_t) i] * (1.0 / 2147483648.0);
            }
            else
            {
                n = decoder->readDouble (tmpDouble.data(), juce::jmin (inFrames, kChunkFrames));
            }
            if (n <= 0) return 0;
            const int out = resampler->process (tmpDouble.data(), n, tmpDouble2.data(), resampler->maxOutputFor (n));
            if (out > 0) { shaper.process (tmpDouble2.data(), out); doublesToRing (tmpDouble2.data(), out); }
            return n;
        }

        case PlaybackPlan::Conversion::DsdToPcm:
        {
            const double ratio = resampler != nullptr ? resampler->getRatio() : 1.0;
            const int pcmFrames = juce::jmax (1, (int) std::floor ((maxOutFrames - 4) / ratio) - 1);
            const int bytesIn = juce::jmin (pcmFrames * dsdConverter->getBytesPerOutputSample(), kChunkFrames * 8);
            const int n = decoder->readDsd (tmpBytes.data(), bytesIn);
            if (n <= 0) return 0;
            int pcm = dsdConverter->process (tmpBytes.data(), n, tmpDouble.data(), dsdConverter->maxOutputFor (n));
            if (resampler != nullptr)
            {
                const int out = resampler->process (tmpDouble.data(), pcm, tmpDouble2.data(), resampler->maxOutputFor (pcm));
                if (out > 0) { shaper.process (tmpDouble2.data(), out); doublesToRing (tmpDouble2.data(), out); }
            }
            else if (pcm > 0)
            {
                shaper.process (tmpDouble.data(), pcm);
                doublesToRing (tmpDouble.data(), pcm);
            }
            return n;
        }
    }
    return 0;
}

/** PCM -> DSD and DSD -> DXD -> DSD, decoder thread: the source becomes DXD 352,8 kHz (64-bit doubles), then the
    5.1 -> 2.0 mix, the decoder's DSP chain (AURA DSP, Studio Tools, plug-ins), the modulator, and DSD bytes go to
    the ring (DoP framing / native DSD at the output, as for a DSF file). No allocation once the chain is prepared.
    Returns the source units consumed (frames, or DSD bytes per channel). */
int PlaybackEngine::produceRemodulated (int maxOutFrames)
{
    const auto& plan = decPlan;
    const int srcCh = plan.source.numChannels;
    const int modCh = plan.downmix ? 2 : srcCh;
    constexpr double dxd = 352800.0;

    if (auto* p = pendingDecChain.exchange (nullptr))
    {
        if (decChain != nullptr) retiredDecChains.write (&decChain, 1);
        decChain = p;
    }
    if (decChain != nullptr && (decChain->preparedRate != dxd || decChain->preparedChannels != modCh))
        decChain->prepare (dxd, modCh, 16384);                                 // decoder thread, not the audio one

    const int bpf = pcmToDsd->getBytesPerInputFrame();
    const int dxdBudget = juce::jlimit (8, 8192, maxOutFrames / bpf - 4);
    double* dx = tmpDouble.data();
    int nd = 0, consumed = 0;
    if (plan.source.isDsd())
    {
        const int bytesIn = juce::jmin (dxdBudget * dsdConverter->getBytesPerOutputSample(), kChunkFrames * 8);
        consumed = decoder->readDsd (tmpBytes.data(), bytesIn);
        if (consumed <= 0) return 0;
        nd = dsdConverter->process (tmpBytes.data(), consumed, tmpDouble.data(), dsdConverter->maxOutputFor (consumed));
    }
    else
    {
        const double ratio = resampler != nullptr ? resampler->getRatio() : 1.0;
        const int inFrames = juce::jlimit (1, kChunkFrames, (int) std::floor ((dxdBudget - 4) / ratio));
        if (plan.source.kind == SampleKind::PcmInt)
        {
            consumed = decoder->readInt (tmpInt.data(), inFrames);
            for (int i = 0; i < consumed * srcCh; ++i) tmpDouble[(size_t) i] = tmpInt[(size_t) i] * (1.0 / 2147483648.0);
        }
        else consumed = decoder->readDouble (tmpDouble.data(), inFrames);
        if (consumed <= 0) return 0;
        nd = consumed;
        if (resampler != nullptr)
        {
            nd = resampler->process (tmpDouble.data(), consumed, tmpDouble2.data(), resampler->maxOutputFor (consumed));
            dx = tmpDouble2.data();
        }
    }
    if (nd <= 0) return consumed;
    if (plan.downmix) foldToStereo (dx, nd, srcCh);
    if (decoderDspOn())
    {
        // planar, in place, then back to interleaved
        std::array<double*, 8> ptr {};
        for (int c = 0; c < modCh; ++c)
        {
            ptr[(size_t) c] = decPlanar.data() + (size_t) c * 16384;
            for (int i = 0; i < nd; ++i) ptr[(size_t) c][i] = dx[(size_t) i * (size_t) modCh + (size_t) c];
        }
        decChain->process (ptr.data(), modCh, nd);
        for (int c = 0; c < modCh; ++c)
            for (int i = 0; i < nd; ++i) dx[(size_t) i * (size_t) modCh + (size_t) c] = ptr[(size_t) c][i];
    }
    if (remodFadeIn > 0)
    {
        // fresh start (not a gapless join): 10 ms fade-in in DXD, before the modulator
        const int total = (int) (transitionFadeMs * 0.001 * 352800.0);
        for (int i = 0; i < nd && remodFadeIn > 0; ++i, --remodFadeIn)
        {
            const double g = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (double) (total - remodFadeIn) / total);
            for (int c = 0; c < modCh; ++c) dx[(size_t) i * (size_t) modCh + (size_t) c] *= g;
        }
    }
    uint8_t* mod = tmpBytes.data() + tmpBytes.size() / 2;                      // second half: modulator output
    const int bytes = pcmToDsd->process (dx, nd, mod);
    stats.modulatorLimited.fetch_add (std::exchange (pcmToDsd->limitedFrames, 0), std::memory_order_relaxed);
    stats.modulatorResets.fetch_add (std::exchange (pcmToDsd->loopResets, 0), std::memory_order_relaxed);
    if (plan.channels == modCh && plan.identityRoute()) writeAll (mod, (size_t) bytes * (size_t) modCh);
    else
    {
        routeChannels (mod, tmpBytes.data(), bytes, modCh, plan.channels, plan.route, (uint8_t) kDsdIdle);
        writeAll (tmpBytes.data(), (size_t) bytes * (size_t) plan.channels);
    }
    return consumed;
}

void PlaybackEngine::drainConverters()
{
    if (decHavePlan && pcmToDsd != nullptr)
    {
        // end of the stream: silence through the resampler and the modulator (look-ahead + interpolator delay)
        const int srcCh = decPlan.source.numChannels, modCh = decPlan.downmix ? 2 : srcCh;
        std::vector<double> dx;
        int nd = 0;
        if (dsdConverter != nullptr)
        {
            const int bytes = dsdConverter->getNumTaps() / 8 + 8;
            std::vector<uint8_t> idle ((size_t) bytes * (size_t) srcCh, kDsdIdle);
            dx.resize ((size_t) (dsdConverter->maxOutputFor (bytes) + 8) * (size_t) srcCh);
            nd = dsdConverter->process (idle.data(), bytes, dx.data(), dsdConverter->maxOutputFor (bytes));
            if (decPlan.downmix) foldToStereo (dx.data(), nd, srcCh);
        }
        else if (resampler != nullptr)
        {
            const int zeros = (int) std::ceil (resampler->getLatencyInputSamples()) + 2;
            std::vector<double> in ((size_t) zeros * (size_t) srcCh, 0.0);
            dx.resize ((size_t) (resampler->maxOutputFor (zeros) + 8) * (size_t) srcCh);
            nd = resampler->process (in.data(), zeros, dx.data(), resampler->maxOutputFor (zeros));
            if (decPlan.downmix) foldToStereo (dx.data(), nd, srcCh);
        }
        const int tail = pcmToDsd->getLatencyInputFrames();
        dx.resize ((size_t) (nd + tail) * (size_t) juce::jmax (srcCh, modCh), 0.0);
        std::fill (dx.begin() + (std::ptrdiff_t) ((size_t) nd * (size_t) modCh), dx.end(), 0.0);
        std::vector<uint8_t> mod ((size_t) (nd + tail) * (size_t) pcmToDsd->getBytesPerInputFrame() * (size_t) modCh);
        const int bytes = pcmToDsd->process (dx.data(), nd + tail, mod.data());
        std::vector<uint8_t> out ((size_t) bytes * (size_t) decPlan.channels);
        routeChannels (mod.data(), out.data(), bytes, modCh, decPlan.channels, decPlan.route, (uint8_t) kDsdIdle);
        writeAll (out.data(), out.size());
        resampler.reset();
        dsdConverter.reset();
        pcmToDsd.reset();
        return;
    }
    if (! decHavePlan || (dsdConverter == nullptr && resampler == nullptr)) return;
    const int srcCh = decPlan.source.numChannels;
    const int outCh = decPlan.channels;

    auto writeDoubles = [&] (std::vector<double>& pcm, int frames)
    {
        std::vector<int32_t> ints ((size_t) frames * (size_t) outCh);
        if (decPlan.downmix)
        {
            foldToStereo (pcm.data(), frames, srcCh);
            for (size_t i = 0; i < ints.size(); ++i) ints[i] = doubleToInt32 (pcm[i]);
            writeAll (ints.data(), ints.size() * 4);
            return;
        }
        for (int i = 0; i < frames; ++i)
            for (int o = 0; o < outCh; ++o)
            {
                const int c = decPlan.route[(size_t) o];
                ints[(size_t) i * outCh + o] = c >= 0 && c < srcCh ? doubleToInt32 (pcm[(size_t) i * srcCh + c]) : 0;
            }
        writeAll (ints.data(), ints.size() * 4);
    };

    // Feed silence through the filters so the end of the music (their group delay) reaches the DAC.
    std::vector<double> pcm;
    int frames = 0;
    if (dsdConverter != nullptr)
    {
        const int bytes = dsdConverter->getNumTaps() / 8 + 8;
        std::vector<uint8_t> idle ((size_t) bytes * (size_t) srcCh, kDsdIdle);
        pcm.resize ((size_t) (dsdConverter->maxOutputFor (bytes) + 8) * (size_t) srcCh);
        frames = dsdConverter->process (idle.data(), bytes, pcm.data(), dsdConverter->maxOutputFor (bytes));
    }
    if (resampler != nullptr)
    {
        const int zeros = (int) std::ceil (resampler->getLatencyInputSamples()) + 2;
        std::vector<double> in ((size_t) (frames + zeros) * (size_t) srcCh, 0.0);
        std::copy (pcm.begin(), pcm.begin() + (std::ptrdiff_t) ((size_t) frames * (size_t) srcCh), in.begin());
        std::vector<double> out ((size_t) (resampler->maxOutputFor (frames + zeros) + 8) * (size_t) srcCh);
        frames = resampler->process (in.data(), frames + zeros, out.data(), resampler->maxOutputFor (frames + zeros));
        pcm.swap (out);
    }
    if (frames > 0) { shaper.process (pcm.data(), frames); writeDoubles (pcm, frames); }
    dsdConverter.reset();
    resampler.reset();
}

//==============================================================================
// Audio thread
//==============================================================================
bool PlaybackEngine::consumeEvents (const PlanWire& active) noexcept
{
    // 1. after a seek / new queue: drop stale events and bytes up to the new generation's first event
    const auto fg = flushGen.load (std::memory_order_acquire);
    if (fg != consumerGen)
    {
        RingEvent e;
        while (events.peek (&e, 1) == 1)
        {
            if (e.gen == fg)
            {
                const auto stale = e.startByte - consumedBytes;
                if (stale > 0) ring.discard ((size_t) stale);
                consumedBytes = e.startByte;
                consumerGen = fg;
                haveCurrent = false;
                prerolling = true;
                prerollFrames = 0;
                break;
            }
            events.discard (1);
        }
        if (consumerGen != fg) return false;
    }

    // 2. events that start here
    RingEvent e;
    while (events.peek (&e, 1) == 1 && e.gen == consumerGen && e.startByte <= consumedBytes)
    {
        if (e.kind == RingEvent::End)
        {
            events.discard (1);
            haveCurrent = false;
            endedFlag = true;
            return false;
        }
        if (e.kind == RingEvent::Position)
        {
            events.discard (1);
            positionBase.store (e.startSeconds);
            positionUnits.store (0);
            unitsIntoTrack = 0;
            continue;
        }
        const auto& w = planWires[(size_t) (e.planId & 63)];
        if (! w.same (active))
        {
            reconfigurePlanId.store (e.planId);
            return false;
        }
        events.discard (1);
        currentEvent = e;
        haveCurrent = true;
        unitsIntoTrack = 0;
        currentIndex.store (e.itemIndex);
        positionBase.store (e.startSeconds);
        positionUnits.store (0);
        positionUnitsPerSecond.store (w.unitsPerSecond);
        playingPlanId.store (e.planId);
        trackChangedFlag = true;
    }
    return haveCurrent;
}

bool PlaybackEngine::keepsDopCarrier() const noexcept
{
    if (! deviceRunning) return false;
    const auto& w = planWires[(size_t) (activePlanId.load() & 63)];
    return w.valid && w.wire == WireFormat::DoP;
}

bool PlaybackEngine::checkPreroll (size_t unitBytes, double unitsPerSecond, int requestedUnits) noexcept
{
    if (! prerolling) return true;
    // Wait for 150 ms of data (or the end of the queue, or 1 s at most) before starting after a seek.
    // Cold DoP start (device just opened): 800 ms of data, never more than half the ring.
    const double wantedSeconds = coldPreroll.load (std::memory_order_relaxed) ? 0.8 : 0.15;
    const auto wanted = std::min ((size_t) (wantedSeconds * unitsPerSecond) * unitBytes, ring.capacity() / 2);
    RingEvent pendingEvents[4];
    const auto numPending = events.peek (pendingEvents, 4);
    bool endQueued = false;
    for (size_t i = 0; i < numPending; ++i) endQueued |= pendingEvents[i].kind == RingEvent::End;
    // A forced minimum (DoP "Sync Safe" after a rate change) keeps sending DoP silence while the network
    // clock (PTP) locks, before the first byte of music.
    // The 30-stable-buffers gate never blocks forever: after 2 s past the minimum the music starts anyway.
    const bool minimumDone = prerollFrames >= prerollMinUnits
                             && (stableCallbacks >= requiredStableCallbacks
                                 || prerollFrames > prerollMinUnits + (juce::int64) (2.0 * unitsPerSecond));
    if (minimumDone && (ring.readable() >= wanted || endQueued || prerollFrames > (juce::int64) (2.0 * unitsPerSecond) + prerollMinUnits))
    {
        prerolling = false;
        prerollMinUnits = 0;
        coldPreroll.store (false, std::memory_order_relaxed);
        return true;
    }
    prerollFrames += requestedUnits;
    return false;
}

void PlaybackEngine::outputSilenceInt (int32_t* const* ch, int numCh, int n) noexcept
{
    const auto& active = planWires[(size_t) (activePlanId.load() & 63)];
    if (active.valid && active.wire == WireFormat::DoP)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto w = dsp::DopPacker::word (dop.nextMarker(), kDsdIdle, kDsdIdle);
            for (int c = 0; c < numCh; ++c) ch[c][i] = w;
        }
        return;
    }
    for (int c = 0; c < numCh; ++c)
        std::memset (ch[c], 0, sizeof (int32_t) * (size_t) n);
}

void PlaybackEngine::renderInt (int32_t* const* out, int numCh, int numFrames) noexcept
{
    stats.callbacks.fetch_add (1, std::memory_order_relaxed);
    {
        const auto u = stats.underruns.load (std::memory_order_relaxed);
        if (numFrames == lastCallbackFrames && u == lastUnderruns) { if (stableCallbacks < 1000000) ++stableCallbacks; }
        else stableCallbacks = 0;
        lastCallbackFrames = numFrames;
        lastUnderruns = u;
    }

    if (auto* p = pendingChain.exchange (nullptr))
    {
        if (activeChain != nullptr) retiredChains.write (&activeChain, 1);
        activeChain = p;
    }

    const auto& active = planWires[(size_t) (activePlanId.load() & 63)];
    int done = 0;

    const bool pcmWire = active.valid && active.wire == WireFormat::Pcm;
    const bool fadeable = pcmWire && fadesOn.load (std::memory_order_relaxed);
    const double fadeLen = juce::jmax (1.0, transitionFadeMs * 0.001 * juce::jmax (1.0, active.rate));
    while (done < numFrames)
    {
        // Micro-fades (PCM only: a DSD stream cannot be scaled). A pause, a stop, a device reconfiguration or a skip /
        // seek (new generation in the ring) first plays the music still in the ring with the gain going down to 0
        // over 10 ms; music after any silence comes back up over 10 ms. Gapless joins have no silence: untouched.
        bool fadingOut = false;
        if (state.load() != State::Playing || reconfigurePlanId.load() != 0)
        {
            if (! fadeable || ! haveCurrent || xfGain <= 0.0) break;
            fadingOut = true;
        }
        else if (flushGen.load (std::memory_order_acquire) != consumerGen && fadeable && haveCurrent && xfGain > 0.0)
            fadingOut = true;                                   // the old music, before consumeEvents() drops it
        else if (! consumeEvents (active))
            break;

        const int ringCh = active.channels;
        const bool isDop = active.wire == WireFormat::DoP;
        const int unitBytes = isDop ? ringCh * 2 : ringCh * 4;   // DoP: two DSD bytes per channel per frame

        if (! fadingOut && ! checkPreroll ((size_t) unitBytes, active.rate, numFrames - done))
            break;

        juce::int64 limit = std::numeric_limits<juce::int64>::max();
        RingEvent next;
        const bool haveNext = events.peek (&next, 1) == 1;
        if (haveNext && (next.gen == consumerGen || fadingOut)) limit = next.startByte - consumedBytes;
        // a natural end of track into another device format (the device will be reopened): fade the last 10 ms
        double untilSwitch = -1.0;
        if (fadeable && ! fadingOut && haveNext && next.gen == consumerGen && ! planWires[(size_t) (next.planId & 63)].same (active))
            untilSwitch = (double) (limit / unitBytes);

        int frames = juce::jmin (numFrames - done, maxBlock);
        frames = (int) std::min<juce::int64> (frames, limit / unitBytes);
        if (fadingOut) frames = juce::jmin (frames, (int) std::ceil (xfGain * fadeLen));
        frames = (int) std::min<juce::int64> (frames, (juce::int64) (ring.readable() / (size_t) unitBytes));
        if (frames <= 0)
        {
            if (limit > 0 && limit < unitBytes)   // odd tail before a boundary: skip it
            {
                ring.discard ((size_t) limit);
                consumedBytes += limit;
                continue;
            }
            if (limit != 0) stats.underruns.fetch_add (1, std::memory_order_relaxed);
            break;
        }

        const size_t bytes = (size_t) frames * (size_t) unitBytes;
        ring.read (rtBytes.data(), bytes);
        consumedBytes += (juce::int64) bytes;

        if (isDop)
        {
            const uint8_t* b = rtBytes.data();
            for (int i = 0; i < frames; ++i)
            {
                const uint8_t marker = dop.nextMarker();
                for (int c = 0; c < numCh; ++c)
                {
                    const int sc = juce::jmin (c, ringCh - 1);
                    out[c][done + i] = dsp::DopPacker::word (marker, b[(size_t) (2 * i) * ringCh + sc], b[(size_t) (2 * i + 1) * ringCh + sc]);
                }
            }
            // analyser: bit density of the first two channels
            for (int i = 0; i < frames * 2; ++i)
                for (int c = 0; c < 2; ++c)
                    rtTap[(size_t) i * 2 + c] = (float) (popcount8 (b[(size_t) i * ringCh + juce::jmin (c, ringCh - 1)]) - 4) * 0.25f;
            analyzer.isDsdDensity = true;
            analyzer.rate = active.rate * 2.0;
            analyzer.push (rtTap.data(), frames * 2);
            unitsIntoTrack += frames * 2;
        }
        else
        {
            const auto* src = reinterpret_cast<const int32_t*> (rtBytes.data());
            const double targetGain = isFixedVolume() ? 1.0 : volume.load();
            const bool dspOn = activeChain != nullptr && activeChain->isActive();
            const int exactBits = deviceExactBits.load();
            const bool ramp = fadeable && (fadingOut || xfGain < 1.0 || (untilSwitch >= 0.0 && untilSwitch < fadeLen + frames));
            const bool process = active.needsRequantize || dspOn || targetGain != 1.0 || smoothedGain != 1.0
                                 || exactBits < active.ringBits || ramp;

            if (! process)
            {
                for (int c = 0; c < numCh; ++c)
                {
                    const int sc = juce::jmin (c, ringCh - 1);
                    for (int i = 0; i < frames; ++i) out[c][done + i] = src[(size_t) i * ringCh + sc];
                }
            }
            else
            {
                const int pc = juce::jmin (ringCh, 8);
                for (int c = 0; c < pc; ++c)
                    for (int i = 0; i < frames; ++i)
                        planarPtrs[(size_t) c][i] = src[(size_t) i * ringCh + c] * (1.0 / 2147483648.0);

                if (targetGain != 1.0 || smoothedGain != 1.0 || ramp)
                {
                    const double coeff = 1.0 - std::exp (-1.0 / (0.010 * juce::jmax (1.0, active.rate)));
                    const double step = 1.0 / fadeLen;
                    auto shape = [] (double x) { return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * juce::jlimit (0.0, 1.0, x)); };
                    for (int i = 0; i < frames; ++i)
                    {
                        smoothedGain += (targetGain - smoothedGain) * coeff;
                        if (std::abs (smoothedGain - targetGain) < 1e-9) smoothedGain = targetGain;
                        double g = smoothedGain;
                        if (ramp)
                        {
                            xfGain = fadingOut ? juce::jmax (0.0, xfGain - step) : juce::jmin (1.0, xfGain + step);
                            g *= shape (xfGain);
                            if (untilSwitch >= 0.0) g *= shape ((untilSwitch - i - 1.0) / fadeLen);
                        }
                        for (int c = 0; c < pc; ++c) planarPtrs[(size_t) c][i] *= g;
                    }
                }
                if (dspOn) activeChain->process (planarPtrs.data(), pc, frames);

                for (int c = 0; c < numCh; ++c)
                {
                    const int sc = juce::jmin (c, pc - 1);
                    auto& rq = requantizers[(size_t) sc];
                    for (int i = 0; i < frames; ++i) out[c][done + i] = rq.process (planarPtrs[(size_t) sc][i]);
                }
            }

            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                    rtTap[(size_t) i * 2 + c] = (float) (out[juce::jmin (c, numCh - 1)][done + i] * (1.0 / 2147483648.0));
            analyzer.isDsdDensity = false;
            analyzer.rate = active.rate;
            analyzer.push (rtTap.data(), frames);
            unitsIntoTrack += frames;
        }

        positionUnits.store (unitsIntoTrack, std::memory_order_relaxed);
        done += frames;
        if (pcmWire && ! fadingOut) xfResetArmed = true;
        if (fadingOut && xfGain <= 0.0) break;                  // silent now: the rest of the callback is silence
    }

    if (done < numFrames)
    {
        // Silent point. After a discontinuity (skip, seek, stop, new device format; not a pause) the DSP forgets
        // the previous music here, at gain 0, so nothing of it rings into what comes next.
        const bool discontinuity = flushGen.load (std::memory_order_relaxed) != consumerGen || reconfigurePlanId.load() != 0
                                   || state.load() == State::Stopped;
        if (discontinuity && xfResetArmed && activeChain != nullptr) { activeChain->reset(); xfResetArmed = false; }
        xfGain = 0.0;
        int32_t* rest[32];
        for (int c = 0; c < juce::jmin (numCh, 32); ++c) rest[c] = out[c] + done;
        outputSilenceInt (rest, juce::jmin (numCh, 32), numFrames - done);
    }
    xfSilent.store (done < numFrames, std::memory_order_relaxed);
    stats.framesOut.fetch_add ((juce::uint64) numFrames, std::memory_order_relaxed);
}

void PlaybackEngine::renderDsd (uint8_t* const* out, int numCh, int numBytes) noexcept
{
    stats.callbacks.fetch_add (1, std::memory_order_relaxed);
    const auto& active = planWires[(size_t) (activePlanId.load() & 63)];
    int done = 0;

    while (done < numBytes)
    {
        if (state.load() != State::Playing || reconfigurePlanId.load() != 0 || ! consumeEvents (active))
            break;

        const int ringCh = active.channels;
        if (! checkPreroll ((size_t) ringCh, active.rate / 8.0, numBytes - done))
            break;
        juce::int64 limit = std::numeric_limits<juce::int64>::max();
        RingEvent next;
        if (events.peek (&next, 1) == 1 && next.gen == consumerGen) limit = next.startByte - consumedBytes;

        int n = juce::jmin (numBytes - done, maxBlock);
        n = (int) std::min<juce::int64> (n, limit / ringCh);
        n = (int) std::min<juce::int64> (n, (juce::int64) (ring.readable() / (size_t) ringCh));
        if (n <= 0)
        {
            if (limit != 0) stats.underruns.fetch_add (1, std::memory_order_relaxed);
            break;
        }

        ring.read (rtBytes.data(), (size_t) n * (size_t) ringCh);
        consumedBytes += (juce::int64) n * ringCh;
        for (int c = 0; c < numCh; ++c)
        {
            const int sc = juce::jmin (c, ringCh - 1);
            for (int i = 0; i < n; ++i) out[c][done + i] = rtBytes[(size_t) i * ringCh + sc];
        }
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
                rtTap[(size_t) i * 2 + c] = (float) (popcount8 (rtBytes[(size_t) i * ringCh + juce::jmin (c, ringCh - 1)]) - 4) * 0.25f;
        analyzer.isDsdDensity = true;
        analyzer.rate = active.rate / 8.0;
        analyzer.push (rtTap.data(), n);
        unitsIntoTrack += n;
        positionUnits.store (unitsIntoTrack, std::memory_order_relaxed);
        done += n;
    }

    for (int c = 0; c < numCh; ++c)
        std::memset (out[c] + done, kDsdIdle, (size_t) (numBytes - done));
    stats.framesOut.fetch_add ((juce::uint64) numBytes, std::memory_order_relaxed);
}

//==============================================================================
// Message thread
//==============================================================================
bool PlaybackEngine::configureDevice (int planId)
{
    PlaybackPlan plan;
    EngineSettings s;
    juce::String dev;
    {
        std::lock_guard<std::mutex> l (controlLock);
        auto it = plans.find (planId);
        if (it == plans.end()) return false;
        plan = it->second;
        s = resolveProfile (settings, deviceName);
        dev = deviceName;
    }

    if (deviceRunning) { backend->stop(); deviceRunning = false; }

    OutputConfig cfg;
    cfg.deviceName = dev;
    cfg.wire = plan.wire;
    cfg.sampleRate = plan.deviceRate;
    cfg.numChannels = plan.channels;
    cfg.sourceBits = plan.ringBitsHint;
    cfg.exclusive = s.exclusive && ! sharedFallback;
    // A manual buffer (ms) always wins; "Automatique" gets 2048 frames for DoP and for Merging / Anubis / RAVENNA.
    cfg.bufferFrames = s.bufferMilliseconds > 0 ? (int) (s.bufferMilliseconds * plan.deviceRate / 1000.0)
                     : s.bufferFrames > 0 ? s.bufferFrames
                     : (plan.wire == WireFormat::DoP || isMergingDevice (dev)) ? 2048 : 0;

    juce::String err;
    if (! backend->open (cfg, err))
    {
        {
            std::lock_guard<std::mutex> l (controlLock);
            lastError = "Sortie audio : " + err;
        }
        int expected = planId;
        reconfigurePlanId.compare_exchange_strong (expected, 0);
        state = State::Stopped;
        if (onError) onError ("Sortie audio : " + err);
        if (onStateChanged) onStateChanged();
        return false;
    }

    const auto status = backend->getStatus();
    const int exact = plan.wire == WireFormat::Pcm ? juce::jlimit (8, 32, status.exactBits > 0 ? status.exactBits : 24) : 24;
    deviceExactBits = exact;
    for (auto& r : requantizers) r.setBits (exact);
    profileLocksVolume = s.lockVolume;
    smoothedGain = isFixedVolume() ? 1.0 : volume.load();

    // The device is stopped: the DSP chain can be (re)prepared and adopted directly.
    if (auto* p = pendingChain.exchange (nullptr)) activeChain = p;
    if (activeChain != nullptr) activeChain->prepare (status.sampleRate > 0 ? status.sampleRate : plan.deviceRate, plan.channels, maxBlock);

    dop.markerToggle = false;
    coldPreroll.store (false, std::memory_order_relaxed);
    stableCallbacks = 0;
    lastCallbackFrames = 0;
    lastUnderruns = stats.underruns.load();
    requiredStableCallbacks = 0;
    // The device is stopped: the audio-thread state can be set directly. DoP: real DSD silence first
    // (never PCM zeros), at least 400 ms, then wait for 30 stable hardware buffers.
    if (plan.wire == WireFormat::DoP)
    {
        prerolling = true;
        prerollFrames = 0;
        prerollMinUnits = (juce::int64) (juce::jmax (s.dopPrerollMs, kDopMinPrerollMs) * plan.deviceRate / 1000.0);
        requiredStableCallbacks = kDopStableCallbacks;
        coldPreroll.store (true, std::memory_order_relaxed);
    }
    activePlanId = planId;
    int expected = planId;
    reconfigurePlanId.compare_exchange_strong (expected, 0);
    stats.reconfigurations.fetch_add (1);

    deviceRunning = backend->start (this);
    pathChangedFlag = true;
    if (! deviceRunning)
    {
        audioLog ("Moteur : le peripherique n'a pas demarre (plan " + juce::String (planId) + ")");
        abortDeviceStart ("Sortie audio : le périphérique n'a pas démarré (journal : "_t + audioLogFile().getFullPathName() + ")");
        return false;
    }
    startedAtMs = juce::Time::getMillisecondCounter();
    callbacksAtStart = stats.callbacks.load();
    watchStage = 0; watchRetries = 0; watchConfirmed = false;
    audioLog ("Moteur : flux demarre, " + formatRate (plan.deviceRate) + (plan.wire == WireFormat::DoP ? " DoP" : "")
              + ", tampon " + juce::String (backend->getStatus().bufferFrames));
    return true;
}

/** The stream never started or the device never clocks it: say so (the timeline would otherwise sit at 0:00)
    and let go of the DAC. */
void PlaybackEngine::abortDeviceStart (const juce::String& why)
{
    {
        std::lock_guard<std::mutex> l (controlLock);
        lastError = why;
    }
    state = State::Stopped;
    if (deviceRunning) { backend->stop(); deviceRunning = false; }
    backend->close();
    activePlanId = 0;
    startedAtMs = 0;
    errorFlag = true;
    if (onStateChanged) onStateChanged();
}

/** After a start, wait for real callbacks (a hardware clock that really runs), log the first seconds, and restart
    the stream up to twice when nothing arrives: this replaces guessing with a measurement. */
void PlaybackEngine::watchDeviceStart()
{
    if (startedAtMs == 0) return;
    if (! deviceRunning || state.load() == State::Stopped) { startedAtMs = 0; return; }

    const auto now = juce::Time::getMillisecondCounter();
    const auto elapsed = now - startedAtMs;
    const auto cbs = stats.callbacks.load() - callbacksAtStart;
    static constexpr juce::uint32 stages[] = { 500, 1000, 2500, 5000 };   // last one after the 4 s check
    while (watchStage < 4 && elapsed >= stages[watchStage])
    {
        audioLog ("Moteur : +" + juce::String ((int) stages[watchStage]) + " ms, callbacks " + juce::String ((juce::int64) cbs)
                  + ", sous-alimentations " + juce::String ((juce::int64) stats.underruns.load())
                  + ", position " + juce::String (getPositionSeconds(), 2) + " s, ring " + juce::String ((juce::int64) ring.readable()) + " octets");
        ++watchStage;
    }
    if (! backend->isRealtime()) { if (watchStage >= 4) startedAtMs = 0; return; }

    if (! watchConfirmed && cbs >= 8)
    {
        watchConfirmed = true;
        audioLog ("Moteur : flux confirme, " + juce::String ((juce::int64) cbs) + " callbacks en " + juce::String ((int) elapsed) + " ms");
    }
    // No hardware buffer yet. Recovery ladder (logs of 16 and 21 Nov 2026: Anubis after a switch to DoP, FiiO KA17 at
    // 384 kHz): 1. restart the stream; 2. close the device (Hog Mode released, formats restored) and open it again;
    // 3. PCM only: the shared system output (a mixer, not bit-perfect, but sound); 4. give up with what to do.
    // DoP gets twice the patience: the VAD reconfigures itself in DSD (PTP lock) and pauses the stream meanwhile.
    const auto plan = getCurrentPlan();
    const bool dopNow = plan.wire == WireFormat::DoP;
    if (! watchConfirmed && elapsed >= (dopNow ? 2 * startPatienceMs : startPatienceMs))
    {
        const int planId = activePlanId;
        const int step = watchRetries + 1;
        const auto devName = [this] { std::lock_guard<std::mutex> l (controlLock); return deviceName; }();
        const auto giveUp = [&]
        {
            audioLog ("Moteur : aucun callback apres " + juce::String (watchRetries) + " tentatives de reprise, abandon");
            abortDeviceStart ("Le DAC « "_t + devName + " » ne répond plus (aucun buffer demandé). Débranchez-le puis rebranchez-le "
                              "(ou éteignez-le / rallumez-le), puis relancez la lecture. Réglages › À propos › Exporter les journaux si ça persiste."_t);
        };
        if (watchRetries == 0)
        {
            audioLog ("Moteur : " + juce::String ((juce::int64) cbs) + " callbacks en " + juce::String ((int) elapsed) + " ms, redemarrage du flux (essai 1)");
            backend->stop();
            stableCallbacks = 0; lastCallbackFrames = 0;       // the device is stopped: audio-thread state can be set directly
            deviceRunning = backend->start (this);
            if (! deviceRunning) { giveUp(); return; }
            startedAtMs = juce::Time::getMillisecondCounter();
            callbacksAtStart = stats.callbacks.load();
            watchStage = 0;
            watchRetries = step;
            return;
        }
        const bool canShare = ! dopNow && ! sharedFallback && plan.wire == WireFormat::Pcm;
        if (watchRetries == 1 || (watchRetries == 2 && canShare))
        {
            if (watchRetries == 1) audioLog ("Moteur : toujours aucun callback, fermeture complete du peripherique et reouverture (essai 2)");
            else
            {
                sharedFallback = true;
                audioLog ("Moteur : toujours aucun callback en acces exclusif, sortie systeme partagee (essai 3)");
            }
            if (deviceRunning) { backend->stop(); deviceRunning = false; }
            backend->close();                                  // Hog Mode released, formats put back
            juce::Thread::sleep (250);
            if (! configureDevice (planId)) return;            // configureDevice reported the error
            watchRetries = step;                               // configureDevice restarted the watch from zero
            if (sharedFallback)
            {
                std::lock_guard<std::mutex> l (controlLock);
                lastError = "Le DAC ne répondait pas en accès exclusif : sortie système partagée pour cette session (pas bit-perfect). "
                            "Débranchez / rebranchez le DAC puis choisissez-le à nouveau pour revenir en exclusif."_t;
                errorFlag = true;
            }
            return;
        }
        giveUp();
        return;
    }
    if (watchStage >= 4 && watchConfirmed) startedAtMs = 0;
}

/** After the start watch, for the whole playback: logs every dropout (ring empty) and every hardware stall (the DAC
    asks for nothing for 300 ms or more), with the position and the ring level. This is what tells a starved decoder
    (dropouts, ring near 0) from a device that stops clocking (stall, ring full) when another application starts. */
void PlaybackEngine::monitorPlayback()
{
    const auto u = stats.underruns.load();
    const auto cb = stats.callbacks.load();
    const auto now = juce::Time::getMillisecondCounter();
    if (state.load() != State::Playing || ! deviceRunning || startedAtMs != 0 || ! backend->isRealtime())
    {
        monUnderruns = u; monCallbacks = cb; monStallSince = 0;
        return;
    }
    if (cb == monCallbacks)
    {
        if (monStallSince == 0) monStallSince = now;
    }
    else
    {
        if (monStallSince != 0 && now - monStallSince >= 300)
            audioLog ("Moteur : blocage du DAC, aucun buffer demande pendant " + juce::String ((int) (now - monStallSince))
                      + " ms (position " + juce::String (getPositionSeconds(), 2) + " s, ring " + juce::String ((juce::int64) ring.readable()) + " octets)");
        monStallSince = 0;
        monCallbacks = cb;
    }
    if (u != monUnderruns && now - monLastGlitchLog >= 1000)
    {
        audioLog ("Moteur : coupure, " + juce::String ((juce::int64) (u - monUnderruns)) + " sous-alimentation(s) (total "
                  + juce::String ((juce::int64) u) + "), position " + juce::String (getPositionSeconds(), 2) + " s, ring "
                  + juce::String ((juce::int64) ring.readable()) + " octets, decodeur " + (decoderBusy.load() ? "actif" : "en attente"));
        monUnderruns = u;
        monLastGlitchLog = now;
    }
}

void PlaybackEngine::timerCallback()
{
    DspChain* retired = nullptr;
    while (retiredChains.read (&retired, 1) == 1 || retiredDecChains.read (&retired, 1) == 1)
        for (auto it = chainStore.begin(); it != chainStore.end(); ++it)
            if (it->get() == retired) { chainStore.erase (it); break; }

    const int rp = reconfigurePlanId.load();
    if (rp != 0 && state.load() != State::Stopped)
        configureDevice (rp);

    watchDeviceStart();
    monitorPlayback();

    // DoP carrier idling in Stopped: release the DAC (Hog Mode, MT VAD) after 10 s. Paused keeps it, resume stays instant.
    if (state.load() == State::Stopped && keepsDopCarrier())
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (stoppedAtMs == 0) stoppedAtMs = now;
        else if (now - stoppedAtMs >= 10000)
        {
            backend->stop();
            deviceRunning = false;
            backend->close();
            activePlanId = 0;
            stoppedAtMs = 0;
        }
    }
    else stoppedAtMs = 0;

    if (trackChangedFlag.exchange (false))
    {
        pathChangedFlag = true;
        if (onTrackChanged) onTrackChanged();
    }
    if (endedFlag.exchange (false))
    {
        state = State::Stopped;
        if (! keepsDopCarrier())
        {
            if (deviceRunning) { backend->stop(); deviceRunning = false; }
            backend->close();
            activePlanId = 0;
        }
        if (onQueueFinished) onQueueFinished();
        if (onStateChanged) onStateChanged();
    }
    if (errorFlag.exchange (false) && onError)
        onError (getLastError());
    if (pathChangedFlag.exchange (false) && onSignalPathChanged)
        onSignalPathChanged();
    stats.clipped = [this] { juce::uint64 c = 0; for (auto& r : requantizers) c += r.clipped; return c; }();
}

//==============================================================================
SignalPath PlaybackEngine::getSignalPath() const
{
    SignalPath path;
    const auto plan = getCurrentPlan();
    const auto status = backend->getStatus();
    const int idx = currentIndex.load();
    const auto item = idx >= 0 && idx < (int) queue.size() ? queue[(size_t) idx] : PlayableItem();

    if (plan.source.sampleRate <= 0 || ! status.open)
    {
        path.summary = "Aucune lecture";
        return path;
    }

    const auto& src = plan.source;
    path.add ("Source", src.describe(), item.sourceName.isNotEmpty() ? item.sourceName : juce::String ("Fichier local"),
              src.lossy ? PathQuality::LowQuality : PathQuality::BitPerfect);

    if (src.isDsd())
        path.add ("Décodage"_t, src.codec + " -> flux 1 bit", "Lecture directe, aucune conversion", PathQuality::BitPerfect);
    else if (src.kind == SampleKind::PcmFloat)
        path.add ("Décodage"_t, src.codec + " -> 64 bits flottant", src.lossy ? "Décodeur avec pertes"_t : "Échantillons flottants"_t,
                  src.lossy ? PathQuality::LowQuality : PathQuality::HighQuality);
    else
        path.add ("Décodage"_t, src.codec + " -> entiers " + juce::String (src.bitsPerSample) + " bits", "Bit à bit, sans passer par le flottant"_t,
                  PathQuality::BitPerfect);

    if (plan.downmix)
        path.add ("Canaux"_t, juce::String (src.numChannels) + " -> 2 (mixage stéréo ITU)"_t,
                  juce::String (src.isDsd() ? "Calculé en DXD 64 bits. "_t : "") + "L = AvG + 0,707 C + 0,707 SurrG (idem à droite), LFE omis, normalisé : sans écrêtage"_t,
                  PathQuality::Enhanced);
    else if (plan.stereoLayer)
        path.add ("Canaux"_t, "Couche stéréo du SACD"_t, "Le mixage stéréo du disque, lu tel quel à la place de la zone multicanal"_t, PathQuality::BitPerfect);
    else if (src.numChannels > 2 && ! plan.identityRoute())
    {
        juce::StringArray map;
        for (int o = 0; o < plan.channels; ++o)
            if (plan.route[(size_t) o] >= 0) map.add (juce::String (plan.route[(size_t) o] + 1) + juce::String::fromUTF8 ("\xe2\x86\x92") + juce::String (o + 1));
        path.add ("Canaux"_t, juce::String (src.numChannels) + " canaux, correspondance des sorties"_t, map.joinIntoString ("  "), PathQuality::BitPerfect);
    }

    switch (plan.conversion)
    {
        case PlaybackPlan::Conversion::DsdToPcm:
        {
            const auto eff = getEffectiveSettings();
            path.add ("Conversion", "DSD -> PCM " + formatRate (plan.deviceRate),
                      (plan.filterShape == FilterShape::MinimumPhase ? juce::String ("FIR phase minimale 64 bits, 150 dB"_t) : juce::String ("FIR phase linéaire 64 bits, 150 dB"_t))
                          + (eff.dsdToPcmGainDb != 0.0 ? ", gain +"_t + juce::String (eff.dsdToPcmGainDb, 0) + " dB" + (eff.limiter ? " + limiteur"_t : juce::String()) : juce::String()),
                      PathQuality::Enhanced);
            break;
        }
        case PlaybackPlan::Conversion::Resample:
            path.add ("Conversion", "Sinc " + formatRate (src.sampleRate) + " -> " + formatRate (plan.deviceRate),
                      plan.filterShape == FilterShape::MinimumPhase ? juce::String ("Polyphase 64 bits, phase minimale (sans pré-écho)"_t)
                                                                    : juce::String ("Polyphase 64 bits, 160 dB"), PathQuality::Enhanced);
            break;
        case PlaybackPlan::Conversion::PcmToDsd:
        case PlaybackPlan::Conversion::DsdRemodulate:
        {
            // the decoder's chain mirrors the audio thread's one (same processors): its state is shown from here
            const auto eff = getEffectiveSettings();
            const auto dsdName = "DSD" + juce::String ((int) std::lround (plan.conversionRate / 44100.0));
            if (plan.conversion == PlaybackPlan::Conversion::DsdRemodulate)
                path.add ("Conversion", src.codec + " -> DXD 352,8 kHz", "FIR 64 bits, 150 dB (le DSD d'origine n'est plus transmis tel quel)"_t, PathQuality::Enhanced);
            else if (src.sampleRate != 352800.0)
                path.add ("Conversion", "PCM " + formatRate (src.sampleRate) + " -> DXD 352,8 kHz", "Sinc polyphase 64 bits, 160 dB"_t, PathQuality::Enhanced);
            if (activeChain != nullptr && activeChain->isActive())
                path.add ("DSP", activeChain->names().joinIntoString (" · "_t), "64 bits en DXD, avant le modulateur"_t, PathQuality::Enhanced);
            path.add ("Modulateur", "DXD -> " + dsdName,
                      "Sigma-delta 1 bit, ordre "_t + juce::String (eff.sdmOrder <= 5 ? 5 : 7) + ", 64 bits, marge "_t
                          + juce::String (eff.pcmToDsdHeadroomDb, 0) + " dB, limiteur de crêtes (plafond -0,9 dB SACD)"_t,
                      PathQuality::Enhanced);
            break;
        }
        case PlaybackPlan::Conversion::FloatToInt:
        case PlaybackPlan::Conversion::None:
            break;
    }

    {
        // NX engine: 64-bit pipeline, Memory Play (whole file in RAM) or disk reads through the 8 MB ring
        path.add ("Moteur", "Moteur Aura 64 bits"_t + (plan.inMemory ? " · Memory Play"_t : juce::String()),
                  plan.inMemory ? "Fichier entier en RAM ("_t + juce::String ((double) plan.fileBytes / 1048576.0, 1) + " Mo) : le disque reste au repos"_t
                                : juce::String ("Lecture disque / réseau, tampon circulaire sans verrou de 8 Mo"_t),
                  PathQuality::BitPerfect);
        const auto eff = getEffectiveSettings();
        if (eff.warmth && plan.profileTag.isNotEmpty()
            && (plan.conversion == PlaybackPlan::Conversion::DsdToPcm || plan.conversion == PlaybackPlan::Conversion::Resample))
            path.add ("Modelage", "Harmoniques paires −90 dB · plateau −0,5 dB > 18 kHz"_t, "64 bits, débrayable dans les réglages"_t, PathQuality::Enhanced);
    }
    if (plan.wire == WireFormat::DoP)
        path.add ("Transport", "DoP " + formatRate (plan.deviceRate) + " · 24 bits"_t, "Marqueurs 0x05 / 0xFA, données DSD intactes"_t, PathQuality::BitPerfect);
    if (plan.wire == WireFormat::NativeDsd)
        path.add ("Transport", "DSD natif " + formatRate (plan.deviceRate), "ASIO DSD", PathQuality::BitPerfect);

    if (plan.wire == WireFormat::Pcm)
    {
        if (! isFixedVolume() && volume.load() != 1.0)
            path.add ("Volume", juce::String (juce::Decibels::gainToDecibels (volume.load()), 1) + " dB",
                      "Numérique 64 bits + dither TPDF"_t, PathQuality::HighQuality);
        if (activeChain != nullptr && activeChain->isActive())
            path.add ("DSP", activeChain->names().joinIntoString (" · "_t), "Plug-ins DSP 64 bits", PathQuality::Enhanced);
        const bool enhancing = plan.conversion == PlaybackPlan::Conversion::Resample || plan.conversion == PlaybackPlan::Conversion::DsdToPcm
                               || (activeChain != nullptr && activeChain->isActive());
        if (plan.needsRequantize || (! isFixedVolume() && volume.load() != 1.0) || (activeChain != nullptr && activeChain->isActive()))
            path.add ("Quantification", juce::String (deviceExactBits.load()) + " bits", "Dither TPDF",
                      enhancing ? PathQuality::Enhanced : PathQuality::HighQuality);
    }

    if (plan.fixedRate)
        path.add ("Fréquence"_t, "Fixe : "_t + formatRate (plan.deviceRate),
                  "Sortie sans fil (Bluetooth / AirPlay) ou mode « Fixe » : la fréquence réglée par le système est gardée, "
                  "aucune renégociation entre les morceaux"_t,
                  plan.deviceRate == src.sampleRate ? PathQuality::BitPerfect : PathQuality::Enhanced);
    path.add ("Sortie", status.backendName + " · "_t + status.deviceName,
              status.detail.isNotEmpty() ? status.detail : (formatRate (status.sampleRate) + " · "_t + juce::String (status.deviceBits) + " bits"),
              status.exclusive && status.osMixerBypassed ? PathQuality::BitPerfect : PathQuality::LowQuality);

    // DAC and the active device profile (Merging Anubis, RME ADI-2...)
    {
        const auto family = activeFamily (getSettings(), status.deviceName.isNotEmpty() ? status.deviceName : deviceName);
        const auto dac = status.deviceName.isNotEmpty() ? status.deviceName : deviceName;
        path.add ("DAC", dac, connectionLabel (dac, status.backendName) + (status.exclusive ? " · accès exclusif"_t : juce::String()),
                  status.exclusive && status.osMixerBypassed ? PathQuality::BitPerfect : PathQuality::LowQuality);
        if (plan.profileTag.isNotEmpty() && family != DeviceFamily::None)
        {
            const auto eff = getEffectiveSettings();
            const int idx = familyCurrentProfile (eff, family);
            path.add ("Profil", profileName (family, idx), profileDescription (family, idx),
                      plan.conversion == PlaybackPlan::Conversion::None || plan.conversion == PlaybackPlan::Conversion::FloatToInt
                          ? PathQuality::BitPerfect : PathQuality::Enhanced);
        }
    }

    switch (path.overall)
    {
        case PathQuality::BitPerfect:  path.summary = "Bit-perfect : le DAC reçoit exactement les données du fichier"_t; break;
        case PathQuality::Enhanced:    path.summary = "Amélioré : traitement 64 bits volontaire"_t; break;
        case PathQuality::HighQuality: path.summary = "Haute qualité : sans perte, mais pas bit-perfect"_t; break;
        case PathQuality::LowQuality:  path.summary = "Qualité réduite : source avec pertes ou mixeur du système"_t; break;
    }
    return path;
}

} // namespace nx
