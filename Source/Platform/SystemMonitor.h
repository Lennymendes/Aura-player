// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Computer card of the hardware dashboard: OS, CPU load, memory in use, and the "no idle sleep while
// playing" assertion (macOS IOKit power assertion, Windows SetThreadExecutionState).
#pragma once

#include <juce_core/juce_core.h>
#include <memory>

namespace nx
{

struct SystemSnapshot
{
    juce::String osName, cpuName, machine;
    int cpuCores = 0;
    double cpuLoad = -1.0;          // 0..1 for the whole machine, −1 = unknown
    juce::int64 memTotal = 0;       // bytes
    juce::int64 memoryUsed = -1;       // bytes, −1 = unknown
    juce::int64 appMemory = -1;     // resident size of Aura, bytes (−1 = unknown)
};

class SystemMonitor
{
public:
    SystemMonitor();
    ~SystemMonitor();
    /** Samples the counters (CPU load is measured between two calls: call it about once a second). */
    SystemSnapshot sample();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** macOS: while music plays, an NSProcessInfo "latency critical" activity keeps App Nap and timer coalescing away
    from the decoder thread and the interface (a game in front, Aura hidden). Other systems: nothing to do. */
class PlaybackActivity
{
public:
    ~PlaybackActivity() { setPlaying (false); }
    void setPlaying (bool playing);
    bool isHeld() const noexcept { return token != nullptr; }
private:
    void* token = nullptr;
};

/** Keeps the computer awake while audio plays (the display may still sleep). */
class SleepBlocker
{
public:
    SleepBlocker();
    ~SleepBlocker();
    void setActive (bool shouldBlock);
    bool isActive() const noexcept { return active; }
    /** False on systems where Aura cannot hold such an assertion (Linux without a session API). */
    static bool isSupported();
private:
    bool active = false;
    juce::uint32 macAssertion = 0;
};

} // namespace nx
