// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Artwork shown full size over the window (album page, Now Playing): click anywhere, the close button or Esc closes it.
#pragma once

#include "AppContext.h"
#include "Widgets.h"

namespace nx
{

class ArtworkViewer : public juce::Component, private juce::ChangeListener
{
public:
    explicit ArtworkViewer (AppContext& c);
    ~ArtworkViewer() override;
    void open (const juce::String& artworkKey, const juce::String& title, const juce::String& subtitle);
    void close();
    /** The square the artwork is drawn in (tests). */
    juce::Rectangle<int> getImageBounds() const noexcept { return imageBounds; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override { close(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }
    IconButton closeButton;
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    AppContext& ctx;
    juce::String key, title, subtitle;
    juce::Image placeholder;
    juce::Rectangle<int> imageBounds;
};

} // namespace nx
