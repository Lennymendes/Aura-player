// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Windows.h"

namespace nx
{

MiniPlayer::MiniPlayer (AppContext& c)
    : ctx (c), artwork (c, 256),
      prev (style::Icon::Previous, "Précédent"_t, c), play (style::Icon::Play, "Lecture / pause"_t, c),
      next (style::Icon::Next, "Suivant"_t, c), expand (style::Icon::Expand, "Fenêtre principale"_t, c), seek (c)
{
    setWantsKeyboardFocus (true);
    artwork.shadow = false;
    artwork.cornerRadius = 8.0f;
    play.primary = true;
    seek.showTimes = false;
    for (auto* comp : std::initializer_list<juce::Component*> { &artwork, &prev, &play, &next, &expand, &seek }) addAndMakeVisible (comp);
    prev.onClick = [this] { ctx.engine().previous(); };
    next.onClick = [this] { ctx.engine().next(); };
    play.onClick = [this] { ctx.engine().togglePlayPause(); };
    expand.onClick = [this] { ctx.toggleMiniPlayer(); };
    seek.onSeek = [this] (double s) { ctx.engine().seek (s); };
    startTimerHz (20);
    trackChanged();
}

void MiniPlayer::trackChanged()
{
    const auto* t = ctx.currentTrack();
    artwork.setArtwork (t ? t->artworkKey : juce::String(), t ? t->album : juce::String ("NX"));
    blurred = {};
    if (t != nullptr)
    {
        auto art = ctx.artwork().loadNow (t->artworkKey, 128);
        if (art.isValid())
        {
            blurred = imaging::coverFit (art, 96, 36).convertedToFormat (juce::Image::RGB);
            imaging::blur (blurred, 8);
        }
    }
    repaint();
}

void MiniPlayer::timerCallback()
{
    play.setIcon (ctx.engine().getState() == PlaybackEngine::State::Playing ? style::Icon::Pause : style::Icon::Play);
    const auto* t = ctx.currentTrack();
    seek.setPosition (ctx.engine().getPositionSeconds(), t ? t->duration : 0.0);
}

void MiniPlayer::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    g.fillAll (t.bg());
    if (blurred.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (blurred, getLocalBounds().toFloat());
    }
    g.setColour (t.colour ("--nx-glass-strong", juce::Colour (0xcc0a0d10)));
    g.fillAll();
    auto r = getLocalBounds().reduced (12);
    r.removeFromLeft (r.getHeight() + 12);
    auto text = r.removeFromTop (r.getHeight() / 2);
    const auto* tr = ctx.currentTrack();
    g.setColour (t.textCol());
    g.setFont (style::font (14.0f, true));
    g.drawText (tr ? tr->title : juce::String ("Aura"), text.removeFromTop (text.getHeight() / 2), juce::Justification::bottomLeft, true);
    g.setColour (t.textDim());
    g.setFont (style::font (12.5f));
    g.drawText (tr ? tr->displayArtist() : juce::String(), text, juce::Justification::topLeft, true);
}

void MiniPlayer::resized()
{
    auto r = getLocalBounds().reduced (12);
    artwork.setBounds (r.removeFromLeft (r.getHeight()));
    r.removeFromLeft (12);
    r.removeFromTop (r.getHeight() / 2);
    seek.setBounds (r.removeFromBottom (10));
    auto controls = r;
    const int b = juce::jmin (36, controls.getHeight());
    expand.setBounds (controls.removeFromRight (b));
    next.setBounds (controls.removeFromRight (b));
    play.setBounds (controls.removeFromRight (b));
    prev.setBounds (controls.removeFromRight (b));
}

//==============================================================================
NowPlayingOverlay::NowPlayingOverlay (AppContext& c) : ctx (c)
{
    setOpaque (true);
    startTimerHz (2);        // a light poll (artwork arriving, track change): nothing animates here
    trackChanged();
}

void NowPlayingOverlay::trackChanged()
{
    const auto* t = ctx.currentTrack();
    shownKey = t != nullptr ? t->artworkKey + "|" + juce::String (t->id) + t->title : juce::String();
    cover = t != nullptr ? ctx.artwork().loadNow (t->artworkKey, 256) : juce::Image();
    blurred = {};
    if (cover.isValid())
    {
        blurred = imaging::coverFit (cover, 120, 30).convertedToFormat (juce::Image::RGB);
        imaging::blur (blurred, 8);
    }
    repaint();
}

void NowPlayingOverlay::timerCallback()
{
    const auto* t = ctx.currentTrack();
    const auto key = t != nullptr ? t->artworkKey + "|" + juce::String (t->id) + t->title : juce::String();
    if (key != shownKey || (t != nullptr && ! cover.isValid() && ctx.artwork().get (t->artworkKey, 256).isValid())) trackChanged();
}

void NowPlayingOverlay::paint (juce::Graphics& g)
{
    const auto& th = ctx.theme();
    auto r = getLocalBounds().toFloat();
    g.fillAll (th.bg());
    if (blurred.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);
        g.drawImage (blurred, r);
    }
    // frosted glass: a translucent white veil, a soft top highlight and a fine specular border
    g.setColour (th.colour ("--nx-glass-strong", juce::Colour (0xcc0a0d10)).withMultipliedAlpha (0.85f));
    g.fillRect (r);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), 0, r.getY(), juce::Colours::white.withAlpha (0.0f), 0, r.getY() + r.getHeight() * 0.6f, false));
    g.fillRect (r);
    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.drawRect (r, 1.0f);

    auto inner = r.reduced (14.0f);
    auto art = inner.removeFromLeft (inner.getHeight());
    juce::Path p;
    p.addRoundedRectangle (art, 8.0f);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (p);
        const auto* t = ctx.currentTrack();
        const auto img = cover.isValid() ? cover : imaging::placeholder (t ? t->album : juce::String ("NX"), 128, th.accent(), th.accent2());
        g.setOpacity (1.0f);
        g.drawImage (img, art);
    }
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.strokePath (p, juce::PathStrokeType (1.0f));

    inner.removeFromLeft (14.0f);
    const auto* t = ctx.currentTrack();
    const auto badge = t != nullptr ? t->resolutionBadge() : juce::String();
    auto top = inner.removeFromTop (inner.getHeight() * 0.5f);
    if (badge.isNotEmpty())
    {
        const float w = style::textWidth (style::font (11.0f, true), badge) + 16.0f;
        style::paintBadge (g, top.removeFromRight (w).withSizeKeepingCentre (w, 18.0f), badge,
                           t->isDsd ? style::qualityColour (th, PathQuality::BitPerfect) : th.accent());
        top.removeFromRight (8.0f);
    }
    g.setColour (th.textCol());
    g.setFont (style::font (17.0f, true));
    g.drawText (t != nullptr ? t->title : juce::String ("Aura"), top, juce::Justification::bottomLeft, true);
    g.setColour (th.textDim());
    g.setFont (style::font (13.5f));
    g.drawText (t != nullptr ? t->displayArtist() : juce::String ("En attente de lecture"_t), inner.withTrimmedTop (3.0f), juce::Justification::topLeft, true);
}

OverlayWindow::OverlayWindow (AppContext& ctx)
    : juce::DocumentWindow ("Aura — Now Playing", ctx.theme().bg(), 0)
{
    setUsingNativeTitleBar (false);
    setTitleBarHeight (0);
    overlay = new NowPlayingOverlay (ctx);
    setContentOwned (overlay, false);
    setSize (460, 104);
    setResizable (false, false);
}

//==============================================================================
MainWindow::MainWindow (AppContext& ctx, juce::LookAndFeel& lf)
    : juce::DocumentWindow ("Aura", ctx.theme().bg(), juce::DocumentWindow::allButtons)
{
    setLookAndFeel (&lf);
    setUsingNativeTitleBar (true);
    main = new MainComponent (ctx);
    setContentOwned (main, false);
    setResizable (true, true);
    setResizeLimits (980, 640, 10000, 10000);
    const auto saved = ctx.settings().get ("windowBounds").toString();
    if (saved.isNotEmpty()) restoreWindowStateFromString (saved);
    else centreWithSize (1360, 860);
    setVisible (true);
    main->grabKeyboardFocus();   // space = play / pause straight away
}

MiniWindow::MiniWindow (AppContext& ctx)
    : juce::DocumentWindow ("Aura — mini"_t, ctx.theme().bg(), 0)
{
    setUsingNativeTitleBar (false);
    setTitleBarHeight (0);
    mini = new MiniPlayer (ctx);
    setContentOwned (mini, false);
    setSize (380, 96);
    setAlwaysOnTop (true);
    setResizable (false, false);
}

} // namespace nx
