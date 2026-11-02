// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Streamer / Gaming mode: one global flag read by the interface timers. The audio thread never looks at it.
#pragma once

#include <juce_events/juce_events.h>
#include <atomic>

namespace nx::perf
{

inline std::atomic<bool>& streamerFlag() { static std::atomic<bool> f { false }; return f; }
inline bool streamer() { return streamerFlag().load(); }
inline void setStreamer (bool on) { streamerFlag() = on; }

/** The application is not in front (a game or OBS is): set by the controller's timer. */
inline std::atomic<bool>& backgroundFlag() { static std::atomic<bool> f { false }; return f; }
inline void setBackground (bool on) { backgroundFlag() = on; }

/** Timer rate for the interface: `normal`, or `low` (a few Hz) in Streamer mode, 1 Hz there when the window is
    in the background. */
inline int hz (int normal, int low) { return streamer() ? (backgroundFlag().load() ? 1 : low) : normal; }

/** Called at the top of a timerCallback(): switches the timer to the rate of the current mode. */
inline void retune (juce::Timer& t, int normal, int low)
{
    const int interval = juce::jmax (1, 1000 / juce::jmax (1, hz (normal, low)));
    if (t.getTimerInterval() != interval) t.startTimer (interval);
}

} // namespace nx::perf
