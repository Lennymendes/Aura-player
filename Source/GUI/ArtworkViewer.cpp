// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "ArtworkViewer.h"
#include "Artwork.h"

namespace nx
{

ArtworkViewer::ArtworkViewer (AppContext& c) : closeButton (style::Icon::Close, "Fermer (Échap)"_t, c), ctx (c)
{
    setWantsKeyboardFocus (true);
    setVisible (false);
    closeButton.onClick = [this] { close(); };
    addAndMakeVisible (closeButton);
    ctx.artwork().addChangeListener (this);
}

ArtworkViewer::~ArtworkViewer() { ctx.artwork().removeChangeListener (this); }

void ArtworkViewer::open (const juce::String& k, const juce::String& t, const juce::String& s)
{
    key = k;
    title = t;
    subtitle = s;
    placeholder = {};
    setVisible (true);
    toFront (true);
    resized();
    grabKeyboardFocus();
    repaint();
}

void ArtworkViewer::close()
{
    if (! isVisible()) return;
    setVisible (false);
    if (auto* p = getParentComponent()) p->grabKeyboardFocus();
}

void ArtworkViewer::resized()
{
    closeButton.setBounds (getWidth() - 56, 18, 38, 38);
    const int textH = title.isNotEmpty() || subtitle.isNotEmpty() ? 64 : 0;
    const int d = juce::jmax (80, juce::jmin (getWidth() - 96, getHeight() - 96 - textH));
    imageBounds = juce::Rectangle<int> (d, d).withCentre ({ getWidth() / 2, (getHeight() - textH) / 2 });
}

void ArtworkViewer::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    g.fillAll (juce::Colours::black.withAlpha (0.88f));
    const auto r = imageBounds.toFloat();
    juce::Path p;
    p.addRoundedRectangle (r, 12.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 32, { 0, 12 }).drawForPath (g, p);
    // the full size when it is loaded; meanwhile the smaller one the page already holds, then the placeholder
    auto img = ctx.artwork().get (key, 1280);
    if (! img.isValid()) img = ctx.artwork().get (key, 640);
    if (! img.isValid())
    {
        if (! placeholder.isValid()) placeholder = imaging::placeholder (title, 512, t.accent(), t.accent2());
        img = placeholder;
    }
    g.saveState();
    g.reduceClipRegion (p);
    g.setOpacity (1.0f);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (img, r);
    g.restoreState();
    if (title.isNotEmpty())
    {
        auto line = getLocalBounds().withTop (imageBounds.getBottom() + 14).withHeight (28);
        g.setColour (juce::Colours::white);
        g.setFont (style::font (18.0f, true));
        g.drawText (title, line, juce::Justification::centred, true);
        if (subtitle.isNotEmpty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.65f));
            g.setFont (style::font (14.0f));
            g.drawText (subtitle, line.translated (0, 26), juce::Justification::centred, true);
        }
    }
}

} // namespace nx
