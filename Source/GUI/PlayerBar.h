// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#pragma once

#include "Widgets.h"

namespace nx
{

/** Player bar chip: miniature of the DAC (Merging Anubis, RME ADI-2, generic) + its active profile; click = profile menu. */
class DeviceChip : public juce::Component
{
public:
    explicit DeviceChip (AppContext& c) : ctx (c) { setRepaintsOnMouseActivity (true); setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override { showMenu(); }
    void showMenu();
    /** The profile entries of the menu (tests): name + whether it is the active one. */
    std::vector<std::pair<juce::String, bool>> menuEntries() const;
    void selectEntry (int index);
private:
    AppContext& ctx;
};

class PlayerBar : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit PlayerBar (AppContext& c);
    ~PlayerBar() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { setMouseCursor (juce::MouseCursor::NormalCursor); }
    void trackChanged();
    void stateChanged();
    /** Shuffle / repeat buttons follow the engine (also changed by a "shuffle the collection" or the phone). */
    void syncTransportButtons();
    void refreshPlugins() { pluginSlot.refreshPlugins(); resized(); }
    /** Refresh period of the clock / seek bar (tests: never throttled). */
    int getClockIntervalMs() const { return getTimerInterval(); }

    // A-B loop (button, keys I / O / L, markers on the seek bar)
    /** Keyboard shortcuts: volume ± (slider position, 0..1) and mute / unmute (back to the previous level). */
    void stepVolume (double delta);
    void toggleMute();
    double getVolumePosition() const { return volume.getValue(); }
    void markLoopA();
    double getPendingLoopA() const noexcept { return pendingLoopA; }
    void markLoopB();
    void clearLoop();

    /** Volume slider position 0..1 (NX Remote, keyboard). */
    void setVolumeLevel (double v);
    /** Waveform instead of the progress line (button left of the elapsed time; saved in the settings). */
    void setWaveformShown (bool on);

    std::function<void()> onToggleLyrics, onToggleQueue, onShowSignalPath, onOpenNowPlaying, onImmersive;
    bool shuffle = false, repeat = false;

    IconButton lyricsButton, queueButton, studioButton, immersiveButton;
    DeviceChip deviceChip;
    IconButton shuffleButton, repeatButton;   // follow the engine's shuffle / repeat state (syncTransportButtons)

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // analysis finished
    AppContext& ctx;
    ArtworkView artwork;
    IconButton prevButton, playButton, nextButton, miniButton, volumeButton, waveButton;
    SeekBar seek;
    LoopButton loopButton;
    double pendingLoopA = -1.0;
    void syncLoop();
    QualityBadge badge;
    juce::Slider volume;
    double volumeBeforeMute = 1.0;
    juce::Label volumeLock;              // "Volume Fixe / DoP" over the greyed slider
    PluginSlot pluginSlot;
    juce::Rectangle<int> textArea;
    juce::String title, subtitle, artistText, albumText;
    juce::Rectangle<int> artistRect;     // clickable artist name (opens the artist page)
};

} // namespace nx
