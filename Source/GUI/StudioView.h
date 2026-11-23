// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Studio View (Extensions, Now Playing): quality diagnosis of the track being played.
//   left   DR (TT DR meter), True Peak, Peak / RMS, mastering profile, "Faux Hi-Res" verdict (from TrackAnalyzer, stored
//          with the track), live L / R peak + RMS meters;
//   right  high-resolution spectrum (4096-point FFT, up to the stream's Nyquist) with the 22,05 kHz mark and the band
//          E9DS works on (manual or Smart Ear Comfort) with its live reduction, and a scrolling spectrogram.
// Everything is read on the message thread from the engine's lock-free tap: nothing on the audio thread.
#pragma once

#include "AppContext.h"
#include "Widgets.h"

namespace nx
{

class StudioView : public juce::Component, private juce::Timer
{
public:
    explicit StudioView (AppContext& c);
    void open();
    void close();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override { if (! panel.contains (e.getPosition())) close(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }
    /** The metric lines shown on the left, "Label|Value" (tests). */
    juce::StringArray metricLines() const;
    /** Frequency of the loudest bin of the live spectrum, Hz (tests). */
    double spectrumPeakHz() const;
    bool showsHiResMark() const;
    int spectrogramColumns() const noexcept { return columns; }
    IconButton closeButton;

private:
    void timerCallback() override;
    void scrollSpectrogram();
    float xForHz (double hz, juce::Rectangle<float> r) const;
    AppContext& ctx;
    juce::Rectangle<int> panel, left, spectrumArea, sonoArea;
    juce::Image sono;
    int columns = 0;
};

} // namespace nx
