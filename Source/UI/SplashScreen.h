// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Launch animation: drawn over the main component, the logo and "NX PLAYER" fade in, then the whole splash fades
// out and removes itself after 1.5 s (juce::Timer). Clicks pass through once it starts fading out.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace nx
{

class SplashScreen : public juce::Component, private juce::Timer
{
public:
    static constexpr double durationSeconds = 1.5;

    /** Covers `parent` (kept in sync with its size) and starts the animation. `finished` runs once, at the end. */
    SplashScreen (juce::Component& parent, std::function<void()> finished = {});
    ~SplashScreen() override;

    /** 0 → 1 over the animation (for the tests). */
    double progress() const noexcept { return juce::jlimit (0.0, 1.0, elapsed / durationSeconds); }
    bool isFinished() const noexcept { return finished; }

    void paint (juce::Graphics& g) override;
    void parentSizeChanged() override { setBounds (getParentComponent()->getLocalBounds()); }

private:
    void timerCallback() override;

    std::function<void()> onFinished;
    double startMs = 0.0, elapsed = 0.0;
    bool finished = false;
};

} // namespace nx
