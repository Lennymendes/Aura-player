// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#pragma once

#include "MainComponent.h"

namespace nx
{

class MiniPlayer : public juce::Component, private juce::Timer
{
public:
    explicit MiniPlayer (AppContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void trackChanged();
    void mouseDown (const juce::MouseEvent& e) override { dragger.startDraggingComponent (getTopLevelComponent(), e.getEventRelativeTo (getTopLevelComponent())); }
    void mouseDrag (const juce::MouseEvent& e) override { dragger.dragComponent (getTopLevelComponent(), e.getEventRelativeTo (getTopLevelComponent()), nullptr); }
    void mouseDoubleClick (const juce::MouseEvent&) override { ctx.toggleMiniPlayer(); }
    bool keyPressed (const juce::KeyPress& k) override { return ctx.handleShortcutKey (k); }   // the same shortcuts as the main window
private:
    void timerCallback() override;
    AppContext& ctx;
    ArtworkView artwork;
    IconButton prev, play, next, expand;
    SeekBar seek;
    juce::Image blurred;
    juce::ComponentDragger dragger;
};

/** Streamer / OBS "Now Playing" card: artwork, title, artist and the resolution badge on frosted glass.
    Drag it anywhere; double-click hides it. Capture its window in OBS (Window Capture). */
class NowPlayingOverlay : public juce::Component, private juce::Timer
{
public:
    explicit NowPlayingOverlay (AppContext& c);
    void paint (juce::Graphics&) override;
    void trackChanged();
    void mouseDown (const juce::MouseEvent& e) override { dragger.startDraggingComponent (getTopLevelComponent(), e.getEventRelativeTo (getTopLevelComponent())); }
    void mouseDrag (const juce::MouseEvent& e) override { dragger.dragComponent (getTopLevelComponent(), e.getEventRelativeTo (getTopLevelComponent()), nullptr); }
    void mouseDoubleClick (const juce::MouseEvent&) override { ctx.setNowPlayingOverlay (false); }
private:
    void timerCallback() override;
    AppContext& ctx;
    juce::Image cover, blurred;
    juce::String shownKey;
    juce::ComponentDragger dragger;
};

class OverlayWindow : public juce::DocumentWindow
{
public:
    explicit OverlayWindow (AppContext& ctx);
    void closeButtonPressed() override { setVisible (false); }
    NowPlayingOverlay& getOverlay() { return *overlay; }
private:
    NowPlayingOverlay* overlay = nullptr;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (AppContext& ctx, juce::LookAndFeel& lf);
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    MainComponent& getMain() { return *main; }
private:
    MainComponent* main = nullptr;
};

class MiniWindow : public juce::DocumentWindow
{
public:
    explicit MiniWindow (AppContext& ctx);
    void closeButtonPressed() override { setVisible (false); }
    MiniPlayer& getMini() { return *mini; }
private:
    MiniPlayer* mini = nullptr;
};

} // namespace nx
