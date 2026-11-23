// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// The player: a decoder thread fills a lock-free ring, the device's real-time thread empties it.
//
//   message thread : commands (play, seek, queue...), device (re)configuration, notifications
//   decoder thread : opens tracks, decodes, converts when needed, writes the ring, gapless transitions
//   audio thread   : reads the ring, packs DoP, applies volume / DSP only when asked, feeds the backend
//
// Track boundaries, seeks and format changes travel through a second lock-free queue of events that
// carry the absolute ring position where they take effect, so the audio thread switches tracks on the
// exact sample and drops stale data after a seek without any lock.
#pragma once

#include "PlaybackPlan.h"
#include "SignalPath.h"
#include "DspChain.h"
#include "../Core/SpscRing.h"
#include "../Audio/AudioDecoder.h"
#include "../DSP/Requantizer.h"
#include "../DSP/OutputShaper.h"
#include "DeviceProfiles.h"
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <random>

namespace nx
{

namespace dsp { class DsdToPcm; class Resampler; class PcmToDsd; }

struct PlayableItem
{
    juce::String id;                     // library id (or provider:id)
    juce::String uri;                    // absolute file path, or an http(s) URL
    juce::String title, artist, album;
    juce::String sourceName;             // "Fichiers locaux", "Qobuz", "NAS (UPnP)"...
    double durationHint = 0.0;
    double truePeakDb = std::numeric_limits<double>::quiet_NaN();   // measured True Peak (dBTP), NaN = not yet
    /** Optional: custom stream opener (authenticated streams, caches...). Called on the decoder thread. */
    std::function<std::unique_ptr<juce::InputStream>()> openStream;
};

/** Lock-free tap of what is being played (for visualisers). Written by the audio thread. */
class AnalyzerTap
{
public:
    AnalyzerTap() : ring (1 << 17) {}
    void push (const float* stereo, int frames) noexcept { ring.write (stereo, (size_t) frames * 2); }
    /** Message thread: newest frames, at getSampleRate(). */
    int pull (float* stereo, int maxFrames) noexcept { return (int) ring.read (stereo, (size_t) maxFrames * 2) / 2; }
    double getSampleRate() const noexcept { return rate.load(); }
    std::atomic<double> rate { 44100.0 };
    std::atomic<bool> isDsdDensity { false };   // samples are 1-bit densities, not PCM
private:
    SpscRing<float> ring;
};

class PlaybackEngine : private RenderCallback,
                       private juce::Thread,
                       private juce::Timer
{
public:
    enum class State { Stopped, Playing, Paused, Buffering };

    explicit PlaybackEngine (std::unique_ptr<OutputBackend> backend);
    ~PlaybackEngine() override;

    //== Device ===============================================================
    OutputBackend& getBackend() noexcept { return *backend; }
    void setBackend (std::unique_ptr<OutputBackend> newBackend);
    void setDevice (const juce::String& deviceName);
    juce::String getDeviceName() const { return deviceName; }
    DeviceCapabilities getCapabilities() const;
    /** replanCurrent: the playing track is re-opened with the new settings (from the play position). */
    void setSettings (const EngineSettings& s, bool replanCurrent = true);
    EngineSettings getSettings() const;
    /** The settings in force for the current device (device profile applied, e.g. Merging Anubis). */
    EngineSettings getEffectiveSettings() const;

    //== Queue & transport (message thread) ===================================
    void setQueue (std::vector<PlayableItem> items, int startIndex = 0, bool startPlaying = true);
    void appendToQueue (std::vector<PlayableItem> items);
    const std::vector<PlayableItem>& getQueue() const noexcept { return queue; }
    void playIndex (int index, double startSeconds = 0.0);
    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void next();
    void previous();
    void seek (double seconds);

    //== Play order: shuffle (each track once per pass) and repeat (off / whole queue / current track) ==========
    enum class Repeat { Off, All, One };
    void setShuffle (bool on);
    bool isShuffle() const;
    void setRepeat (Repeat mode);
    Repeat getRepeat() const;
    /** Tracks still to come before the queue (or the shuffled pass) ends, the current one excluded. */
    int getRemainingCount() const;
    /** The track that follows `index` in play order; -1 at the end. Repeat One returns `index` unless userAction. */
    int getNextIndex (int index, bool userAction = false) const;

    //== A-B loop (gapless: the device keeps running, the decoder wraps) =======
    /** Loops [a, b) of the current track for ever (b − a ≥ 50 ms). Re-reads from the play position once. */
    void setLoop (double aSeconds, double bSeconds);
    void clearLoop (bool rebuffer = true);
    bool isLoopActive() const noexcept { return loopIndex.load() >= 0; }
    double getLoopA() const noexcept { return loopA.load(); }
    double getLoopB() const noexcept { return loopB.load(); }
    int getLoopIndex() const noexcept { return loopIndex.load(); }
    /** Crossfade at the wrap point (PCM), in milliseconds. */
    static constexpr double loopCrossfadeMs = 4.0;

    State getState() const noexcept { return state.load(); }
    int getCurrentIndex() const noexcept { return currentIndex.load(); }
    double getPositionSeconds() const noexcept;
    double getDurationSeconds() const;

    //== Volume & DSP =========================================================
    /** Linear gain. 1.0 with fixedVolume keeps the stream bit-perfect. */
    void setVolume (double gain);
    double getVolume() const noexcept { return volume.load(); }
    void setFixedVolume (bool fixed);
    /** Volume held at 100 %: by the user (bit-perfect) or by the device profile (RME DSD Direct safety). */
    bool isFixedVolume() const noexcept { return fixedVolume.load() || profileLocksVolume.load(); }
    bool isVolumeLockedByProfile() const noexcept { return profileLocksVolume.load(); }
    void setDspChain (std::unique_ptr<DspChain> chain);
    /** A second instance of the same chain, for the decoder thread: it processes the DXD signal before the DSD
        modulator (PCM -> DSD, DSD -> DXD -> DSD). Never shared with the audio thread's chain. */
    void setDecoderDspChain (std::unique_ptr<DspChain> chain);
    /** Auto-Headroom True Peak: the measurement of a queued track arrived (message thread). It applies at once to the
        track being played (10 ms glide) and to the next ones from their first sample. */
    void setTrackTruePeak (int queueIndex, double truePeakDb);
    /** dB of attenuation the Auto-Headroom applies to the current track (0 = none; PCM sources only). */
    double getTruePeakAttenuationDb() const;

    //== Status ===============================================================
    PlaybackPlan getCurrentPlan() const;
    OutputStatus getOutputStatus() const { return backend->getStatus(); }
    SignalPath getSignalPath() const;
    AnalyzerTap& getAnalyzer() noexcept { return analyzer; }

    struct Stats
    {
        std::atomic<juce::uint64> underruns { 0 }, callbacks { 0 }, framesOut { 0 }, reconfigurations { 0 }, clipped { 0 };
        // DSD modulator (PCM -> DSD, DSD -> DXD -> DSD): DXD frames held down by its limiter, loop resets (never expected)
        std::atomic<juce::uint64> modulatorLimited { 0 }, modulatorResets { 0 };
    };
    const Stats& getStats() const noexcept { return stats; }
    juce::String getLastError() const;
    size_t getBufferedBytes() const noexcept { return ring.readable(); }
    bool isDecoderBusy() const noexcept { return decoderBusy.load(); }

    /** Message thread notifications. */
    std::function<void()> onTrackChanged, onStateChanged, onQueueFinished, onSignalPathChanged;
    std::function<void (const juce::String&)> onError;
    /** Start of the error reported when Aura Free meets a Studio Master track (DSD, DXD > 192 kHz, SACD). */
    static constexpr const char* studioMasterError = "[Premium] ";

    /** Opens network / provider streams (Brique 2). Called on the decoder thread. Files are opened directly. */
    std::function<std::unique_ptr<juce::InputStream> (const PlayableItem&)> streamResolver;

    /** For tests: run the message-thread housekeeping now instead of waiting for the timer. */
    void serviceMessageThread() { timerCallback(); }
    /** Start watchdog: how long a fresh stream may stay silent (no hardware buffer) before recovery. Tests shorten it. */
    juce::uint32 startPatienceMs = 4000;
    /** Micro-fades at every break of the PCM stream (play, pause, stop, skip, seek, rate change). */
    static constexpr double transitionFadeMs = 10.0;
    /** True after the exclusive (Hog Mode) stream never clocked and the shared system output took over. */
    bool isOnSharedFallback() const noexcept { return sharedFallback; }

private:
    //== RenderCallback (audio thread) ========================================
    void renderInt (int32_t* const* channels, int numChannels, int numFrames) noexcept override;
    void renderDsd (uint8_t* const* channels, int numChannels, int numBytes) noexcept override;

    //== Decoder thread ========================================================
    void run() override;
    bool openItem (int index, double startSeconds, bool flush);
    void preparePlanConverters (const PlaybackPlan& plan, bool keepState);
    int produce (int writableBytes);
    void drainConverters();
    void modulateOut (const double* dxd, int frames) noexcept;  // decoder: modulator -> routing -> ring
    void releaseHeld (bool fadeOut, double* tail = nullptr, int tailFrames = 0);   // decoder: the held last 10 ms go to the modulator
    void writeAll (const void* data, size_t bytes);

    //== Message thread ========================================================
    void timerCallback() override;
    void requestFlushStart (int index, double seconds);
    bool configureDevice (int planId);

    struct PlanWire
    {
        WireFormat wire = WireFormat::Pcm;
        double rate = 0.0;
        int channels = 2;
        int ringBits = 24;
        bool needsRequantize = false;
        double unitsPerSecond = 44100.0;
        bool valid = false;
        bool same (const PlanWire& o) const noexcept
        {
            return valid && o.valid && wire == o.wire && rate == o.rate && channels == o.channels
                && (wire != WireFormat::Pcm || ringBits == o.ringBits);
        }
    };

    struct RingEvent
    {
        enum Kind : int { Track, End, Position } kind = Track;   // Position: the loop wrapped, the clock jumps back
        juce::int64 startByte = 0;
        juce::uint32 gen = 0;
        int itemIndex = 0;
        int planId = 0;
        double startSeconds = 0.0;
        double truePeakDb = std::numeric_limits<double>::quiet_NaN();
        bool truePeakInAudio = false;    // the audio thread applies the Auto-Headroom (PCM as is to the ring)
    };

    bool consumeEvents (const PlanWire& active) noexcept;   // audio thread
    bool checkPreroll (size_t unitBytes, double unitsPerSecond, int requestedUnits) noexcept;
    static void foldToStereo (double* interleaved, int frames, int srcCh) noexcept;
    void outputSilenceInt (int32_t* const* ch, int numCh, int n) noexcept;

    std::unique_ptr<OutputBackend> backend;
    juce::String deviceName;
    DeviceCapabilities caps;
    EngineSettings settings;
    mutable std::mutex controlLock;          // decoder thread <-> message thread only, never the audio thread

    std::vector<PlayableItem> queue;         // message thread copy
    std::vector<PlayableItem> decoderQueue;  // decoder thread copy (under controlLock)
    bool queueDirty = false;
    bool shuffleOn = false;                  // all of these under controlLock
    Repeat repeatMode = Repeat::Off;
    std::vector<int> playOrder;              // shuffle: a permutation of the queue indices
    std::mt19937 orderRng { std::random_device{}() };
    void rebuildOrderLocked (int first);     // [first] then the others shuffled
    int nextLocked (int index, bool userAction);

    // Ring + events
    SpscRing<uint8_t> ring;
    SpscRing<RingEvent> events { 256 };
    std::array<PlanWire, 64> planWires {};
    std::map<int, PlaybackPlan> plans;       // planId -> full plan (controlLock)
    std::atomic<int> nextPlanId { 1 };

    // Commands to the decoder thread
    struct FlushCommand { bool pending = false; int index = 0; double seconds = 0.0; juce::uint32 gen = 0; };
    FlushCommand flushCommand;               // controlLock
    std::atomic<juce::uint32> flushGen { 0 };
    std::atomic<bool> stopDecoding { false };

    // Decoder thread state
    std::unique_ptr<AudioDecoder> decoder;
    int decIndex = -1;
    PlaybackPlan decPlan;
    int decPlanId = 0;
    bool decHavePlan = false;
    juce::int64 bytesWritten = 0;
    std::unique_ptr<dsp::DsdToPcm> dsdConverter;
    std::unique_ptr<dsp::Resampler> resampler;
    std::unique_ptr<dsp::PcmToDsd> pcmToDsd;   // PCM -> DSD remodulation (decoder thread, prepared per plan)
    int produceRemodulated (int maxOutFrames);
    int remodFadeIn = 0;                         // decoder: DXD frames still to fade in after a fresh start
    // decoder, PCM / DSD -> DSD: the last 10 ms of DXD are held back from the modulator until the next track is known,
    // so they can be faded out before a change of DSD rate / plan (a DSD stream cannot be faded after the modulator)
    std::vector<double> remodHold;
    int remodHeld = 0;
    // audio thread: transition gain of the micro-fades (0 = silent, 1 = music as is), and the reset of the DSP
    // memories that is due at the next silent point after a discontinuity (skip, seek, stop, new device format)
    double xfGain = 0.0;
    bool xfResetArmed = false;
    std::atomic<bool> xfSilent { true };         // the last callback ended in silence (stop() waits for it)
    std::atomic<bool> fadesOn { true };          // EngineSettings::transitionFades, for the audio thread
    std::atomic<DspChain*> pendingDecChain { nullptr };
    SpscRing<DspChain*> retiredDecChains { 16 };
    DspChain* decChain = nullptr;                // decoder thread
    std::vector<double> decPlanar;               // planar DXD for decChain (preallocated)
    bool decoderDspOn() const noexcept { return decChain != nullptr && decChain->isActive(); }
    dsp::OutputShaper shaper;                 // warmth / limiter after a conversion (device profiles)
    std::vector<int32_t> tmpInt;
    std::vector<double> tmpDouble, tmpDouble2;
    std::vector<uint8_t> tmpBytes;
    juce::uint32 decoderGen = 0;

    // Audio thread state
    juce::uint32 consumerGen = 0;
    juce::int64 consumedBytes = 0;
    RingEvent currentEvent;
    bool haveCurrent = false;
    juce::int64 unitsIntoTrack = 0;
    bool prerolling = false;
    juce::int64 prerollFrames = 0;
    juce::int64 prerollMinUnits = 0;         // forced silence after a (re)configuration (DoP "Sync Safe")
    // DoP lock: the timeline only starts once the hardware callback has delivered this many consecutive
    // identical, underrun-free buffers (the driver / network clock is then stable).
    std::atomic<bool> coldPreroll { false };  // device just opened in DoP: ask for 800 ms in the ring
    // Start watchdog (message thread): the stream counts as started only once real callbacks arrive.
    juce::uint32 startedAtMs = 0;
    juce::uint64 callbacksAtStart = 0;
    int watchStage = 0, watchRetries = 0;
    bool watchConfirmed = false;
    bool sharedFallback = false;             // exclusive access gave no clock: the shared output is used (until another device)
    // Continuous monitor (message thread), after the start watch: dropouts and hardware stalls with their time.
    juce::uint64 monUnderruns = 0, monCallbacks = 0;
    juce::uint32 monStallSince = 0, monLastGlitchLog = 0;
    void monitorPlayback();
    void abortDeviceStart (const juce::String& why);
    void watchDeviceStart();
    juce::uint32 stoppedAtMs = 0;            // message thread: when the DoP carrier started idling in Stopped
    int stableCallbacks = 0;
    int requiredStableCallbacks = 0;
    int lastCallbackFrames = 0;
    juce::uint64 lastUnderruns = 0;
    bool keepsDopCarrier() const noexcept;   // device kept open, sending the DoP silence carrier
    dsp::DopPacker dop;
    std::array<dsp::Requantizer, 8> requantizers;
    double smoothedGain = 1.0;
    // Auto-Headroom True Peak: settings for the realtime threads, and a measurement that arrived during playback
    std::atomic<bool> tpAuto { false };
    std::atomic<double> tpCeiling { -1.0 };
    std::atomic<int> liveTpIndex { -1 };
    std::atomic<double> liveTpDb { 0.0 };
    double tpGainFor (int itemIndex, double eventTpDb) const noexcept;
    // decoder thread: the Auto-Headroom of the track being decoded (conversions: before the resampler / modulator)
    void applyDecoderTruePeak (double* interleaved, int frames, int channels, double rate) noexcept;
    double decTpDb = std::numeric_limits<double>::quiet_NaN();
    bool decTpApply = false, decTpSnap = true;
    double decTpGain = 1.0;
    std::vector<double> planar;              // 8 × maxBlock
    std::array<double*, 8> planarPtrs {};
    std::vector<uint8_t> rtBytes;
    std::vector<float> rtTap;
    static constexpr int maxBlock = 16384;

    // Shared atomics
    std::atomic<State> state { State::Stopped };
    std::atomic<int> currentIndex { -1 };
    std::atomic<double> loopA { -1.0 }, loopB { -1.0 };
    std::atomic<int> loopIndex { -1 };
    class LoopingDecoder* loopDecoder = nullptr;   // decoder thread: the wrapper of the current track, if any
    int loopWrapsSeen = 0;
    std::atomic<int> activePlanId { 0 };     // plan the device is configured for
    std::atomic<int> reconfigurePlanId { 0 };// set when the device must be reopened
    std::atomic<int> playingPlanId { 0 };
    std::atomic<double> positionBase { 0.0 };
    std::atomic<juce::int64> positionUnits { 0 };
    std::atomic<double> positionUnitsPerSecond { 44100.0 };
    std::atomic<bool> trackChangedFlag { false }, endedFlag { false }, pathChangedFlag { false };
    std::atomic<double> volume { 1.0 };
    std::atomic<bool> fixedVolume { true };
    std::atomic<bool> profileLocksVolume { false };
    std::atomic<DspChain*> pendingChain { nullptr };
    SpscRing<DspChain*> retiredChains { 16 };
    DspChain* activeChain = nullptr;         // audio thread (or message thread while the device is stopped)
    std::vector<std::unique_ptr<DspChain>> chainStore;   // message thread owns every chain
    std::atomic<int> deviceExactBits { 24 };
    std::atomic<bool> errorFlag { false };
    std::atomic<bool> decoderBusy { false };
    bool deviceRunning = false;

    AnalyzerTap analyzer;
    Stats stats;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackEngine)
};

} // namespace nx
