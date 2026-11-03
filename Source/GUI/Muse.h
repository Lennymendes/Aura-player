// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// MUSE — Precision Audio Control: the audio chain in one panel (source, headroom, sample rate conversion,
// crossfeed, headphone EQ with presets and its response curve, monitoring). Opened from the studio button of the
// player bar and from the audio status chip of the header.
#pragma once

#include "Widgets.h"

namespace nx
{

class MusePanel : public juce::Component, private juce::Timer
{
public:
    explicit MusePanel (AppContext& c);
    void open();
    void close();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }

    /** Tests: clicks a preset tile / a monitoring segment as the user would. */
    void choosePreset (int index);
    void chooseMonitor (int index);

private:
    void timerCallback() override { repaint(); syncToggles(); }
    void syncToggles();
    juce::Rectangle<int> panelBounds() const;
    AppContext& ctx;
    IconButton closeButton;
    juce::ToggleButton headroom, upsample, crossfeed, eq, phase;
    std::vector<std::pair<juce::Rectangle<int>, int>> presetTiles, monitorSegs;
    juce::Rectangle<int> graphArea, sourceArea;
};

} // namespace nx
