// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Widgets.h"
#include "../Library/Downloads.h"
#include "../Ext/PluginManager.h"
#include "LibraryActions.h"

namespace nx
{

//==============================================================================
void IconButton::paintButton (juce::Graphics& g, bool hi, bool down)
{
    const auto& t = ctx.theme();
    auto r = getLocalBounds().toFloat();
    if (primary)
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight()) - 2.0f;
        auto c = r.withSizeKeepingCentre (d, d);
        g.setColour (t.textCol().withAlpha (down ? 0.85f : 1.0f));
        g.fillEllipse (c);
        style::drawIcon (g, icon, c.reduced (d * 0.28f).translated (icon == style::Icon::Play ? d * 0.03f : 0.0f, 0.0f), t.bg());
        return;
    }
    if ((hi || down) && isEnabled())
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight());
        g.setColour (style::neutral().withAlpha (down ? 0.14f : 0.08f));
        g.fillEllipse (r.withSizeKeepingCentre (d, d));
    }
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * glyph;
    style::drawIcon (g, icon, r.withSizeKeepingCentre (s, s),
                     ! isEnabled() ? t.textCol().withAlpha (0.28f) : active ? t.accent() : (hi ? t.textCol() : t.textCol().withAlpha (0.82f)));
    if (pro) style::drawProBadge (g, { r.getRight(), r.getY() }, t.accent(), t.bg());
    if (one)
    {
        g.setColour (t.accent());
        g.setFont (style::font (10.0f, true));
        g.drawText ("1", r.withSizeKeepingCentre (s, s), juce::Justification::centred);
    }
    if (active)
    {
        g.setColour (t.accent());
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ r.getCentreX(), r.getBottom() - 3.0f }));
    }
}

//==============================================================================
ArtworkView::ArtworkView (AppContext& c, int px) : ctx (c), size (px)
{
    ctx.artwork().addChangeListener (this);
    setInterceptsMouseClicks (false, false);
}

ArtworkView::~ArtworkView() { ctx.artwork().removeChangeListener (this); }

void ArtworkView::setArtwork (const juce::String& k, const juce::String& text)
{
    if (k == key && text == placeholderText) return;
    key = k;
    placeholderText = text;
    placeholderImage = {};
    repaint();
}

void ArtworkView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float d = juce::jmin (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (d, d).reduced (shadow ? 6.0f : 0.0f);
    if (shadow)
    {
        juce::DropShadow ds (juce::Colours::black.withAlpha (0.55f), 18, { 0, 8 });
        juce::Path p;
        p.addRoundedRectangle (r, cornerRadius);
        ds.drawForPath (g, p);
    }
    auto img = ctx.artwork().get (key, size);
    if (! img.isValid())
    {
        if (! placeholderImage.isValid())
            placeholderImage = imaging::placeholder (placeholderText, 256, ctx.theme().accent(), ctx.theme().accent2());
        img = placeholderImage;
    }
    juce::Path clip;
    clip.addRoundedRectangle (r, cornerRadius);
    g.saveState();
    g.reduceClipRegion (clip);
    g.setOpacity (1.0f);   // drawImage uses the current colour's alpha (left at the shadow's)
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (img, r);
    g.restoreState();
    g.setColour (style::neutral().withAlpha (0.06f));
    g.drawRoundedRectangle (r, cornerRadius, 1.0f);
}

//==============================================================================
void QualityBadge::paint (juce::Graphics& g)
{
    if (label.isEmpty()) return;
    const auto c = style::qualityColour (ctx.theme(), quality);
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (c.withAlpha (isMouseOver() ? 0.24f : 0.14f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (c.withAlpha (0.55f));
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
    auto icon = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.22f);
    style::drawIcon (g, style::Icon::Signal, icon, c);
    g.setColour (c);
    g.setFont (style::font (12.0f * uiScale, true));
    g.drawText (label, r.withTrimmedRight (8.0f), juce::Justification::centredLeft);
}

//==============================================================================
double SeekBar::xToTime (float x) const
{
    const auto bar = getLocalBounds().toFloat().reduced (showTimes ? 46.0f : 0.0f, 0.0f);
    return length * juce::jlimit (0.0, 1.0, (double) ((x - bar.getX()) / juce::jmax (1.0f, bar.getWidth())));
}

void SeekBar::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    auto r = getLocalBounds().toFloat();
    if (showTimes)
    {
        g.setColour (t.textDim());
        g.setFont (style::font (11.5f));
        g.drawText (style::formatTime (position), r.removeFromLeft (46.0f), juce::Justification::centred);
        g.drawText (length > 0 ? "-" + style::formatTime (length - position) : juce::String ("--:--"), r.removeFromRight (46.0f), juce::Justification::centred);
    }
    const bool over = isMouseOver() || dragging;
    const float th = over ? 6.0f : 4.0f;
    auto track = r.withSizeKeepingCentre (r.getWidth(), th);
    const float frac = length > 0 ? (float) juce::jlimit (0.0, 1.0, position / length) : 0.0f;
    const bool wave = waveform && analysis != nullptr;
    if (wave)
    {
        painter.paint (g, r.reduced (0.0f, 1.0f), *analysis, frac, t.accent(), t.textCol().withAlpha (0.32f));
        g.setColour (t.textCol());
        g.fillRect (juce::Rectangle<float> (track.getX() + track.getWidth() * frac - 0.75f, r.getY(), 1.5f, r.getHeight()));   // playhead
    }
    else
    {
        g.setColour (style::neutral().withAlpha (0.14f));
        g.fillRoundedRectangle (track, th * 0.5f);
        g.setGradientFill (juce::ColourGradient (t.accent(), track.getX(), 0, t.accent2(), track.getRight(), 0, false));
        g.fillRoundedRectangle (track.withWidth (track.getWidth() * frac), th * 0.5f);
    }

    // A-B loop: highlighted zone and the two draggable markers
    if (length > 0 && loopA >= 0)
    {
        const float xa = timeToX (loopA);
        const float xb = loopB >= 0 ? timeToX (loopB) : xa;
        if (loopB >= 0)
        {
            g.setColour (t.accent().withAlpha (0.28f));
            g.fillRoundedRectangle (juce::Rectangle<float> (xa, track.getY() - 3.0f, juce::jmax (2.0f, xb - xa), track.getHeight() + 6.0f), 3.0f);
        }
        auto marker = [&] (float x, const juce::String& label)
        {
            g.setColour (t.accent());
            g.fillRoundedRectangle (x - 1.0f, track.getY() - 6.0f, 2.0f, track.getHeight() + 12.0f, 1.0f);
            g.setFont (style::font (9.5f, true));
            g.drawText (label, juce::Rectangle<float> (x - 8.0f, r.getY() - 1.0f, 16.0f, 11.0f), juce::Justification::centred);
        };
        marker (xa, "A");
        if (loopB >= 0) marker (xb, "B");
    }
    if (over && length > 0 && ! wave)
    {
        g.setColour (t.textCol());
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ track.getX() + track.getWidth() * frac, track.getCentreY() }));
    }
}

float SeekBar::timeToX (double t) const
{
    const auto bar = getLocalBounds().toFloat().reduced (showTimes ? 46.0f : 0.0f, 0.0f);
    return bar.getX() + bar.getWidth() * (float) (length > 0 ? juce::jlimit (0.0, 1.0, t / length) : 0.0);
}

int SeekBar::markerAt (float x) const
{
    if (length <= 0) return 0;
    if (loopB >= 0 && std::abs (x - timeToX (loopB)) < 7.0f) return 2;
    if (loopA >= 0 && std::abs (x - timeToX (loopA)) < 7.0f) return 1;
    return 0;
}

void SeekBar::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (markerAt ((float) e.x) != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void SeekBar::mouseDown (const juce::MouseEvent& e)
{
    if (length <= 0) return;
    draggingMarker = markerAt ((float) e.x);
    if (draggingMarker != 0) return;
    dragging = true;
    position = xToTime ((float) e.x);
    repaint();
}

void SeekBar::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingMarker == 1) { loopA = juce::jlimit (0.0, loopB >= 0 ? loopB - 0.1 : length, xToTime ((float) e.x)); repaint(); return; }
    if (draggingMarker == 2) { loopB = juce::jlimit (loopA + 0.1, length, xToTime ((float) e.x)); repaint(); return; }
    if (dragging) { position = xToTime ((float) e.x); repaint(); }
}

void SeekBar::mouseUp (const juce::MouseEvent& e)
{
    if (draggingMarker != 0)
    {
        draggingMarker = 0;
        if (onLoopEdited) onLoopEdited (loopA, loopB);
        return;
    }
    if (! dragging) return;
    dragging = false;
    position = xToTime ((float) e.x);
    if (onSeek) onSeek (position);
}

void LoopButton::paintButton (juce::Graphics& g, bool hi, bool down)
{
    const auto& t = ctx.theme();
    auto r = getLocalBounds().toFloat().reduced (1.0f, 5.0f);
    const auto c = stage == 2 ? t.accent() : stage == 1 ? t.accent2() : t.textCol().withAlpha (hi ? 1.0f : 0.8f);
    if (stage == 2 || hi || down)
    {
        g.setColour ((stage == 2 ? t.accent() : juce::Colours::white).withAlpha (stage == 2 ? 0.18f : (down ? 0.14f : 0.08f)));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    }
    g.setColour (c);
    g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.2f);
    g.setFont (style::font (11.5f, true));
    g.drawText (stage == 1 ? juce::String ("A-") : juce::String ("A-B"), r, juce::Justification::centred);
}

//==============================================================================
PluginSlot::PluginSlot (AppContext& c, juce::String s, bool h) : ctx (c), slot (std::move (s)), horizontal (h)
{
    setInterceptsMouseClicks (false, false);
    refreshPlugins();
}

void PluginSlot::refreshPlugins()
{
    ids = ctx.plugins().getEnabledForSlot (slot);
    int fps = 0;
    for (auto& id : ids) fps = juce::jmax (fps, ctx.plugins().getFps (id));
    fpsNormal = fps;
    if (fps > 0) startTimerHz (perf::hz (fps, 2));
    else stopTimer();
    repaint();
}

int PluginSlot::getPreferredHeight() const
{
    int h = 0;
    for (auto& id : ids) h += ctx.plugins().getPreferredHeight (id) + 8;
    return h;
}

void PluginSlot::paint (juce::Graphics& g)
{
    if (ids.isEmpty()) return;
    auto r = getLocalBounds().toFloat();
    if (horizontal)
    {
        const float w = r.getWidth() / (float) ids.size();
        for (auto& id : ids) ctx.plugins().render (id, slot, g, r.removeFromLeft (w).reduced (2.0f));
        return;
    }
    for (auto& id : ids)
    {
        const float h = (float) ctx.plugins().getPreferredHeight (id);
        ctx.plugins().render (id, slot, g, r.removeFromTop (h));
        r.removeFromTop (8.0f);
    }
}

//==============================================================================
AlbumGrid::AlbumGrid (AppContext& c) : ctx (c) { ctx.artwork().addChangeListener (this); }
AlbumGrid::~AlbumGrid() { ctx.artwork().removeChangeListener (this); }

void AlbumGrid::setAlbums (std::vector<AlbumInfo> a)
{
    albums = std::move (a);
    if (libraryAlbums && ! roundArt && sortMode.isNotEmpty()) sortAlbums (albums, sortMode);
    hover = -1; anchor = -1; selected.clear(); repaint();
}

std::vector<AlbumInfo> AlbumGrid::getSelection() const
{
    std::vector<AlbumInfo> out;
    for (int i : selected) if (i >= 0 && i < (int) albums.size()) out.push_back (albums[(size_t) i]);
    return out;
}

void AlbumGrid::selectAll()
{
    selected.clear();
    for (int i = 0; i < (int) albums.size(); ++i) selected.insert (i);
    repaint();
}

int AlbumGrid::columns() const
{
    if (listMode) return 1;
    const float card = cardSize > 0.0f ? cardSize : ctx.theme().number ("--nx-card-size", 176.0f);
    return juce::jmax (1, (int) ((float) (getWidth() + 20) / (card + 20.0f)));
}

juce::Rectangle<int> AlbumGrid::cardBounds (int i) const
{
    if (listMode) return { 0, i * rowHeight, getWidth(), rowHeight - 4 };
    const int cols = columns();
    const int gap = 20;
    const int w = (getWidth() - gap * (cols - 1)) / cols;
    const int h = w + 52;
    return { (i % cols) * (w + gap), (i / cols) * (h + gap), w, h };
}

int AlbumGrid::heightForWidth (int width) const
{
    if (albums.empty()) return 0;
    if (listMode) return (maxRows > 0 ? juce::jmin ((int) albums.size(), maxRows * 4) : (int) albums.size()) * rowHeight;
    const float card = cardSize > 0.0f ? cardSize : ctx.theme().number ("--nx-card-size", 176.0f);
    const int cols = juce::jmax (1, (int) ((float) (width + 20) / (card + 20.0f)));
    int rows = ((int) albums.size() + cols - 1) / cols;
    if (maxRows > 0) rows = juce::jmin (rows, maxRows);
    const int w = (width - 20 * (cols - 1)) / cols;
    return rows * (w + 52 + 20);
}

juce::Rectangle<int> AlbumGrid::editRect (int i) const
{
    if (! onEdit || ! libraryAlbums || roundArt || i < 0 || i >= (int) albums.size()) return {};
    const auto card = cardBounds (i);
    if (listMode) return juce::Rectangle<int> (30, 30).withPosition (card.getRight() - 40, card.getCentreY() - 15);
    return juce::Rectangle<int> (30, 30).withPosition (card.getRight() - 38, card.getY() + 8);
}

int AlbumGrid::indexAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) albums.size(); ++i)
        if (cardBounds (i).contains (p)) return i;
    return -1;
}

void AlbumGrid::mouseMove (const juce::MouseEvent& e)
{
    const int i = indexAt (e.getPosition());
    if (i != hover) { hover = i; repaint(); }
}

void AlbumGrid::mouseDown (const juce::MouseEvent&) { dragStarted = false; }

void AlbumGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || ! libraryAlbums || e.getDistanceFromDragStart() < 8 || e.mods.isPopupMenu()) return;
    const int i = indexAt (e.getMouseDownPosition());
    if (i < 0) return;
    auto dragged = selected.count (i) > 0 ? getSelection() : std::vector<AlbumInfo> { albums[(size_t) i] };
    const auto ids = actions::trackIds (actions::albumTracks (ctx, dragged));
    if (ids.empty()) return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        const auto label = dragged.size() == 1 ? dragged.front().title : juce::String ((int) dragged.size()) + " albums";
        auto img = ctx.artwork().get (dragged.front().artworkKey, 128);
        juce::ScaledImage preview (img.isValid() ? img.rescaled (72, 72) : juce::Image());
        container->startDragging (actions::dragPayload (ids, label), this, preview);
    }
}

void AlbumGrid::mouseUp (const juce::MouseEvent& e)
{
    if (dragStarted) { dragStarted = false; return; }
    const int i = indexAt (e.getPosition());
    if (i < 0) { if (! e.mods.isAnyModifierKeyDown()) clearSelection(); return; }

    if (e.mods.isPopupMenu())
    {
        if (selected.count (i) == 0) { selected.clear(); selected.insert (i); repaint(); }
        if (onContextMenu) onContextMenu (getSelection());
        else if (libraryAlbums) actions::showAlbumMenu (ctx, getSelection(), this);
        return;
    }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        if (selected.count (i) > 0) selected.erase (i); else selected.insert (i);
        anchor = i;
        repaint();
        return;
    }
    if (e.mods.isShiftDown() && anchor >= 0)
    {
        for (int k = juce::jmin (anchor, i); k <= juce::jmax (anchor, i); ++k) selected.insert (k);
        repaint();
        return;
    }
    selected.clear();
    anchor = i;
    if (editRect (i).contains (e.getPosition())) { onEdit (albums[(size_t) i]); return; }
    if (listMode) { if (onOpen) onOpen (albums[(size_t) i]); return; }
    const auto art = cardBounds (i).removeFromTop (cardBounds (i).getWidth());
    const auto playButton = juce::Rectangle<int> (40, 40).withPosition (art.getRight() - 50, art.getBottom() - 50);
    if (playButton.contains (e.getPosition()) && onPlay) onPlay (albums[(size_t) i]);
    else if (onOpen) onOpen (albums[(size_t) i]);
}

void AlbumGrid::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    const int cols = columns();
    const int limit = maxRows > 0 ? juce::jmin ((int) albums.size(), maxRows * (listMode ? 4 : cols)) : (int) albums.size();
    const auto clip = g.getClipBounds();
    for (int i = 0; i < limit; ++i)
    {
        auto card = cardBounds (i);
        if (! card.intersects (clip)) continue;
        if (listMode) { paintRow (g, i, card); continue; }
        const auto& a = albums[(size_t) i];
        auto art = card.removeFromTop (card.getWidth()).toFloat();
        const bool hot = i == hover;
        const bool isSelected = selected.count (i) > 0;
        if (hot) art = art.expanded (3.0f);

        juce::Path p;
        if (roundArt) p.addEllipse (art.reduced (art.getWidth() * 0.04f));
        else p.addRoundedRectangle (art, t.number ("--nx-radius-small", 8.0f));
        juce::DropShadow (juce::Colours::black.withAlpha (hot ? 0.55f : 0.35f), hot ? 22 : 12, { 0, 6 }).drawForPath (g, p);
        auto img = ctx.artwork().get (a.artworkKey, 320);
        if (! img.isValid()) img = imaging::placeholder (a.title, 128, t.accent(), t.accent2());
        g.saveState();
        g.reduceClipRegion (p);
        g.setOpacity (1.0f);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        if (roundArt)
        {
            // fill the circle (crop the longest side)
            const float side = (float) juce::jmin (img.getWidth(), img.getHeight());
            g.drawImage (img, art.getX(), art.getY(), art.getWidth(), art.getHeight(),
                         (int) ((float) (img.getWidth() - side) * 0.5f), (int) ((float) (img.getHeight() - side) * 0.25f), (int) side, (int) side);
        }
        else g.drawImage (img, art);
        if (hot)
        {
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRect (art);
        }
        g.restoreState();
        if (isSelected)
        {
            g.setColour (t.accent());
            g.strokePath (p, juce::PathStrokeType (3.0f));
            auto tick = juce::Rectangle<float> (24, 24).withPosition (art.getRight() - 30, art.getY() + 6);
            g.fillEllipse (tick);
            style::drawIcon (g, style::Icon::Check, tick.reduced (4.0f), t.colour ("--nx-on-accent", juce::Colours::black), 2.6f);
        }

        if (hot && ! isSelected)
        {
            auto pb = juce::Rectangle<float> (40, 40).withPosition (art.getRight() - 50, art.getBottom() - 50);
            g.setColour (t.accent());
            g.fillEllipse (pb);
            style::drawIcon (g, style::Icon::Play, pb.reduced (11.0f).translated (1.5f, 0), t.colour ("--nx-on-accent", juce::Colours::black));
        }

        // badges on the artwork: format, then NAS / Jellyfin
        float bx = art.getX() + 8;
        const auto badge = a.badge();
        if (badge.isNotEmpty() && ! a.lossy && ! roundArt)
        {
            const float bw = (float) badge.length() * 7.0f + 14.0f;
            auto br = juce::Rectangle<float> (bw, 18.0f).withPosition (bx, art.getY() + 8);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (br, 6.0f);
            g.setColour (style::onDark (a.hasDsd ? style::qualityColour (t, PathQuality::BitPerfect) : t.accent()));
            g.setFont (style::font (11.0f, true));
            g.drawText (badge, br, juce::Justification::centred);
            bx = br.getRight() + 5.0f;
        }
        if (const auto chb = channelBadge (a.maxChannels); chb.isNotEmpty() && ! roundArt)
        {
            auto br = juce::Rectangle<float> ((float) chb.length() * 7.0f + 12.0f, 18.0f).withPosition (bx, art.getY() + 8);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (br, 6.0f);
            g.setColour (style::onDark (a.maxChannels > 2 ? t.accent2() : t.textCol()).withAlpha (0.9f));
            g.setFont (style::font (11.0f, true));
            g.drawText (chb, br, juce::Justification::centred);
            bx = br.getRight() + 5.0f;
        }
        if (a.fromNas() && libraryAlbums)
        {
            const auto label = ctx.networkSourceLabel (a.nasSource).toUpperCase();
            const float bw = (float) label.length() * 7.0f + 26.0f;
            auto br = juce::Rectangle<float> (bw, 18.0f).withPosition (bx, art.getY() + 8);
            g.setColour (t.accent2().withAlpha (0.85f));
            g.fillRoundedRectangle (br, 6.0f);
            style::drawIcon (g, style::Icon::Network, br.removeFromLeft (18.0f).reduced (3.0f), juce::Colours::black.withAlpha (0.85f), 2.2f);
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.setFont (style::font (10.5f, true));
            g.drawText (label, br.withTrimmedRight (4.0f), juce::Justification::centred);
        }
        if (a.favourite)
            style::drawIcon (g, style::Icon::Heart, juce::Rectangle<float> (18, 18).withPosition (art.getRight() - 26, art.getBottom() - 26), t.accent());
        if (hot && ! editRect (i).isEmpty())
        {
            const auto er = editRect (i).toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillEllipse (er);
            style::drawIcon (g, style::Icon::Edit, er.reduced (7.0f), juce::Colours::white, 2.0f);
            if (! ctx.isPremium()) style::drawProBadge (g, { er.getRight() + 6.0f, er.getY() - 6.0f }, t.accent(), t.bg());
        }

        card.removeFromTop (8);
        g.setColour (t.textCol());
        g.setFont (style::font (14.0f, true));
        g.drawText (a.title, card.removeFromTop (20), roundArt ? juce::Justification::centred : juce::Justification::centredLeft, true);
        g.setColour (t.textDim());
        g.setFont (style::font (13.0f));
        g.drawText (a.artist + (a.year > 0 ? " · "_t + juce::String (a.year) : juce::String()), card.removeFromTop (18),
                    roundArt ? juce::Justification::centred : juce::Justification::centredLeft, true);
    }
}

void AlbumGrid::paintRow (juce::Graphics& g, int i, juce::Rectangle<int> row)
{
    const auto& t = ctx.theme();
    const auto& a = albums[(size_t) i];
    const bool hot = i == hover, isSelected = selected.count (i) > 0;
    g.setColour (isSelected ? t.accent().withAlpha (0.22f) : style::neutral().withAlpha (hot ? 0.08f : 0.03f));
    g.fillRoundedRectangle (row.toFloat(), 8.0f);
    auto r = row.reduced (8, 4);
    auto art = r.removeFromLeft (r.getHeight()).toFloat();
    auto img = ctx.artwork().get (a.artworkKey, 128);
    if (! img.isValid()) img = imaging::placeholder (a.title, 64, t.accent(), t.accent2());
    juce::Path p;
    p.addRoundedRectangle (art, 6.0f);
    g.saveState();
    g.reduceClipRegion (p);
    g.setOpacity (1.0f);          // drawImage uses the current colour's alpha: the row background above is at 3-8 %
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (img, art);
    g.restoreState();
    r.removeFromLeft (12);
    auto right = r.removeFromRight (editRect (i).isEmpty() ? 8 : 48);
    juce::ignoreUnused (right);

    // right to left: format, channels, duration, tracks
    auto chip = [&] (const juce::String& text, juce::Colour c)
    {
        if (text.isEmpty()) return;
        const float w = (float) juce::roundToInt (style::textWidth (style::font (11.0f, true), text)) + 14.0f;
        style::paintBadge (g, r.removeFromRight ((int) w + 6).withSizeKeepingCentre ((int) w, 18).toFloat(), text, c);
    };
    if (! a.lossy) chip (a.badge(), a.hasDsd ? style::qualityColour (t, PathQuality::BitPerfect) : t.accent());
    chip (channelBadge (a.maxChannels), a.maxChannels > 2 ? t.accent2() : t.textDim());
    g.setColour (t.textDim());
    g.setFont (style::font (12.5f));
    g.drawText (juce::String (a.trackCount) + " " + "morceaux"_t + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + style::formatTime (a.duration), r.removeFromRight (150), juce::Justification::centredRight, true);
    g.setColour (t.textCol());
    g.setFont (style::font (14.0f, true));
    g.drawText (a.title, r.removeFromTop (r.getHeight() / 2 + 2), juce::Justification::bottomLeft, true);
    g.setColour (t.textDim());
    g.setFont (style::font (12.5f));
    g.drawText (a.artist + (a.year > 0 ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + juce::String (a.year) : juce::String()), r, juce::Justification::topLeft, true);
    if (hot && ! editRect (i).isEmpty())
    {
        const auto er = editRect (i).toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (er);
        style::drawIcon (g, style::Icon::Edit, er.reduced (7.0f), juce::Colours::white, 2.0f);
        if (! ctx.isPremium()) style::drawProBadge (g, { er.getRight() + 6.0f, er.getY() - 6.0f }, t.accent(), t.bg());
    }
}

//==============================================================================
void PillBar::setPills (juce::StringArray i, juce::StringArray l, const juce::String& a)
{
    ids = std::move (i);
    labels = std::move (l);
    active = a;
    layoutPills();
    repaint();
}

void PillBar::layoutPills()
{
    bounds.clear();
    int x = 0;
    const auto f = style::font (13.5f, true);
    for (int i = 0; i < ids.size(); ++i)
    {
        auto text = labels[i];
        if (auto it = counts.find (ids[i]); it != counts.end() && it->second > 0) text << "  " << it->second;
        const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, text)) + 30;
        // a thin divider between the views and the quality filters
        if (ids[i] == "dxd") x += 14;
        bounds.push_back ({ x, 0, w, getHeight() });
        x += w + 8;
    }
}

void PillBar::paint (juce::Graphics& g)
{
    layoutPills();
    const auto& t = ctx.theme();
    const auto mouse = getMouseXYRelative();
    for (int i = 0; i < ids.size(); ++i)
    {
        auto r = bounds[(size_t) i].toFloat().reduced (0.5f, 3.0f);
        const bool on = ids[i] == active;
        const bool hot = isMouseOver() && bounds[(size_t) i].contains (mouse);
        const bool quality = ids[i] == "dxd" || ids[i] == "dsd" || ids[i] == "hires" || ids[i] == "cd";
        const auto qc = ids[i] == "dsd" ? style::qualityColour (t, PathQuality::BitPerfect) : ids[i] == "dxd" ? t.accent2() : t.accent();
        if (on)
        {
            g.setColour (quality ? qc : t.textCol());
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        }
        else
        {
            g.setColour (style::neutral().withAlpha (hot ? 0.12f : 0.06f));
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (quality ? qc.withAlpha (0.55f) : t.border());
            g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
        }
        auto text = labels[i];
        if (auto it = counts.find (ids[i]); it != counts.end() && it->second > 0) text << "  " << it->second;
        g.setColour (on ? t.bg() : (quality ? qc : t.textCol()));
        g.setFont (style::font (13.5f, true));
        g.drawText (text, r, juce::Justification::centred);
        if (ids[i] == "dxd" && i > 0)
        {
            g.setColour (t.border());
            g.fillRect ((float) bounds[(size_t) i].getX() - 11.0f, r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);
        }
    }
}

void PillBar::mouseUp (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) bounds.size(); ++i)
        if (bounds[(size_t) i].contains (e.getPosition()))
        {
            active = ids[i];
            repaint();
            if (onSelect) onSelect (ids[i]);
            return;
        }
}

//==============================================================================
TrackList::TrackList (AppContext& c) : ctx (c) { ctx.artwork().addChangeListener (this); ctx.downloads().addChangeListener (this); }
TrackList::~TrackList() { ctx.artwork().removeChangeListener (this); ctx.downloads().removeChangeListener (this); }

void TrackList::download (const std::vector<TrackInfo>& list)
{
    const int n = ctx.downloads().enqueue (list);
    ctx.showMessage (n > 0 ? "Téléchargement de "_t + juce::String (n) + " morceau(x) pour l'écoute hors-ligne"_t
                           : juce::String ("Déjà disponible hors-ligne"_t));
    repaint();
}

void TrackList::setTracks (std::vector<TrackInfo> t, bool art, bool alb, bool num)
{
    tracks = std::move (t);
    artwork = art; album = alb; numbered = num;
    hover = selected = -1;
    artistRects.clear();
    editButtons.clear();
    repaint();
}

void TrackList::mouseMove (const juce::MouseEvent& e)
{
    const int i = e.y / rowHeight;
    if (i != hover) { hover = i < (int) tracks.size() ? i : -1; repaint(); }
    const bool overArtist = i >= 0 && i < (int) artistRects.size() && artistRects[(size_t) i].contains (e.getPosition());
    setMouseCursor (overArtist ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void TrackList::mouseDown (const juce::MouseEvent&) { dragStarted = false; }

void TrackList::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || e.getDistanceFromDragStart() < 8 || e.mods.isPopupMenu()) return;
    const int i = e.getMouseDownY() / rowHeight;
    if (i < 0 || i >= (int) tracks.size() || tracks[(size_t) i].id <= 0) return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        const auto& tr = tracks[(size_t) i];
        auto img = ctx.artwork().get (tr.artworkKey, 96);
        container->startDragging (actions::dragPayload ({ tr.id }, tr.title), this, juce::ScaledImage (img.isValid() ? img.rescaled (56, 56) : juce::Image()));
    }
}

void TrackList::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = e.y / rowHeight;
    if (i >= 0 && i < (int) tracks.size()) ctx.playTracks (tracks, i);
}

void TrackList::mouseUp (const juce::MouseEvent& e)
{
    const int i = e.y / rowHeight;
    if (i < 0 || i >= (int) tracks.size()) return;
    if (dragStarted) { dragStarted = false; return; }
    selected = i;
    repaint();
    const auto track = tracks[(size_t) i];
    if (! e.mods.isPopupMenu())
    {
        if (i < (int) artistRects.size() && artistRects[(size_t) i].contains (e.getPosition()))
        {
            ctx.navigate ({ PageId::ArtistDetail, track.displayArtist() });
            return;
        }
        if (i < (int) downloadButtons.size() && downloadButtons[(size_t) i].contains (e.getPosition()))
            download ({ track });
        if (onEdit && i < (int) editButtons.size() && editButtons[(size_t) i].contains (e.getPosition()) && track.id > 0)
        {
            onEdit (track);
            return;
        }
        if (i < (int) heartButtons.size() && heartButtons[(size_t) i].contains (e.getPosition()) && track.id > 0)
        {
            auto& tr = tracks[(size_t) i];
            tr.favourite = ! tr.favourite;
            ctx.library().setFavourite (tr.id, tr.favourite);
            ctx.showMessage (tr.favourite ? "Ajouté aux favoris"_t : "Retiré des favoris"_t);
            repaint();
            if (onChanged) onChanged();
        }
        return;
    }

    juce::PopupMenu m;
    m.addItem ("Lire"_t, [this, i] { ctx.playTracks (tracks, i); });
    m.addItem ("Ajouter à la file d'attente"_t, [this, track] { ctx.enqueue ({ track }); });
    m.addItem ("Voir l'album"_t, [this, track] { ctx.navigate ({ PageId::AlbumDetail, track.albumArtist.isNotEmpty() ? track.albumArtist : track.artist, track.album }); });
    m.addItem ("Voir l'artiste"_t, [this, track] { ctx.navigate ({ PageId::ArtistDetail, track.displayArtist() }); });
    m.addItem ("Informations…"_t, [this, track] { ctx.showDetails ({ track }); });
    if (track.id > 0)
        m.addItem (track.favourite ? "Retirer des favoris"_t : "Ajouter aux favoris"_t, [this, i]
        {
            auto& tr = tracks[(size_t) i];
            tr.favourite = ! tr.favourite;
            ctx.library().setFavourite (tr.id, tr.favourite);
            repaint();
        });
    if (track.id > 0)
        m.addSubMenu ("Ajouter à une playlist"_t, actions::playlistSubmenu (ctx, { track.id }));
    if (playlistContext > 0)
        m.addItem ("Retirer de cette playlist"_t, [this, i]
        {
            ctx.library().removeFromPlaylist (playlistContext, i);
            tracks.erase (tracks.begin() + i);
            repaint();
            if (onChanged) onChanged();
        });
    m.addItem ("Analyser / Convertir dans AuraEngine X"_t, [this, track] { ctx.sendToAuraEngine ({ track }); });
    if (track.id > 0 && track.source.startsWith ("upnp"))
        m.addItem ("Retirer de la bibliothèque ("_t + ctx.networkSourceLabel (track.source) + ")", [this, track, i]
        {
            ctx.library().remove (track.source, track.externalId);
            tracks.erase (tracks.begin() + i);
            ctx.library().notifyChanged();
            repaint();
        });
    if (DownloadManager::canDownload (track))
    {
        m.addSeparator();
        const bool offline = ctx.downloads().getState (track) == DownloadManager::State::Done;
        m.addItem ("Télécharger pour l'écoute hors-ligne"_t, ! offline, offline, [this, track] { download ({ track }); });
        std::vector<TrackInfo> albumTracks;
        for (auto& t : tracks) if (t.album == track.album && t.source == track.source) albumTracks.push_back (t);
        if (albumTracks.size() > 1)
            m.addItem ("Télécharger l'album « "_t + track.album + " »"_t, [this, albumTracks] { download (albumTracks); });
    }
    if (track.credits.isNotEmpty())
    {
        juce::PopupMenu credits;
        for (auto& line : juce::StringArray::fromLines (track.credits)) credits.addItem (line, false, false, nullptr);
        m.addSubMenu ("Crédits"_t, credits);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void TrackList::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    const auto* playing = ctx.currentTrack();
    const auto clip = g.getClipBounds();
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        auto row = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight);
        if (! row.intersects (clip)) continue;
        const auto& tr = tracks[(size_t) i];
        const bool isPlaying = playing != nullptr && playing->source == tr.source && playing->externalId == tr.externalId;
        if (i == hover || i == selected)
        {
            g.setColour (style::neutral().withAlpha (i == selected ? 0.08f : 0.05f));
            g.fillRoundedRectangle (row.toFloat().reduced (0, 2), 8.0f);
        }
        auto r = row.reduced (10, 6);

        if (numbered)
        {
            auto n = r.removeFromLeft (30);
            if (isPlaying)
            {
                // three animated bars
                const double time = juce::Time::getMillisecondCounterHiRes() / 1000.0;
                for (int b = 0; b < 3; ++b)
                {
                    const float h = 6.0f + 8.0f * (float) (0.5 + 0.5 * std::sin (time * (5.0 + b * 1.7) + b));
                    g.setColour (t.accent());
                    g.fillRoundedRectangle ((float) n.getX() + 4.0f + b * 5.0f, (float) n.getCentreY() + 7.0f - h, 3.0f, h, 1.5f);
                }
            }
            else
            {
                g.setColour (t.textDim());
                g.setFont (style::font (13.0f));
                g.drawText (tr.trackNumber > 0 ? juce::String (tr.trackNumber) : juce::String (i + 1), n, juce::Justification::centredLeft);
            }
        }
        if (artwork)
        {
            auto a = r.removeFromLeft (r.getHeight()).toFloat();
            auto img = ctx.artwork().get (tr.artworkKey, 96);
            if (! img.isValid()) img = imaging::placeholder (tr.album.isNotEmpty() ? tr.album : tr.title, 64, t.accent(), t.accent2());
            juce::Path p;
            p.addRoundedRectangle (a, 5.0f);
            g.saveState();
            g.reduceClipRegion (p);
            g.setOpacity (1.0f);
            g.drawImage (img, a);
            g.restoreState();
            r.removeFromLeft (12);
        }

        auto right = r.removeFromRight (64);
        g.setColour (t.textDim());
        g.setFont (style::font (13.0f));
        g.drawText (style::formatTime (tr.duration), right, juce::Justification::centredRight);

        // metadata pencil (album page), left of the heart
        if ((int) editButtons.size() != (int) tracks.size()) editButtons.assign (tracks.size(), {});
        editButtons[(size_t) i] = {};
        if (onEdit && tr.id > 0)
        {
            auto cell = r.removeFromRight (30).withSizeKeepingCentre (20, 20);
            editButtons[(size_t) i] = cell.expanded (4);
            style::drawIcon (g, style::Icon::Edit, cell.toFloat(), i == hover ? t.textCol() : t.textDim());
            if (! ctx.isPremium()) style::drawProBadge (g, { (float) cell.getRight() + 8.0f, (float) cell.getY() - 7.0f }, t.accent().withAlpha (0.85f), t.bg());
        }

        // favourite heart (click)
        if ((int) heartButtons.size() != (int) tracks.size()) heartButtons.assign (tracks.size(), {});
        heartButtons[(size_t) i] = {};
        if (tr.id > 0)
        {
            auto cell = r.removeFromRight (30).withSizeKeepingCentre (20, 20);
            heartButtons[(size_t) i] = cell.expanded (4);
            if (tr.favourite) style::drawIcon (g, style::Icon::Heart, cell.toFloat(), t.accent());
            else if (i == hover) style::drawIcon (g, style::Icon::HeartOutline, cell.toFloat(), t.textDim());
        }

        const auto badge = tr.resolutionBadge();
        if (badge.isNotEmpty())
        {
            auto b = r.removeFromRight (78).withSizeKeepingCentre (70, 18).toFloat();
            const auto c = tr.lossy ? style::qualityColour (t, PathQuality::LowQuality)
                         : tr.isDsd ? style::qualityColour (t, PathQuality::BitPerfect) : t.accent();
            style::paintBadge (g, b, badge, c);
        }
        if (const auto chb = channelBadge (tr.channels); chb.isNotEmpty())
            style::paintBadge (g, r.removeFromRight (40).withSizeKeepingCentre (34, 18).toFloat(), chb,
                               tr.channels > 2 ? t.accent2() : t.textDim());
        // Offline column: download button / progress ring / "hors-ligne" mark
        if ((int) downloadButtons.size() != (int) tracks.size()) downloadButtons.assign (tracks.size(), {});
        downloadButtons[(size_t) i] = {};
        const auto dlState = ctx.downloads().getState (tr);
        if (DownloadManager::canDownload (tr) || dlState == DownloadManager::State::Done)
        {
            auto cell = r.removeFromRight (34).withSizeKeepingCentre (26, 26);
            const auto cf = cell.toFloat();
            switch (dlState)
            {
                case DownloadManager::State::Done:
                {
                    g.setColour (t.accent().withAlpha (0.18f));
                    g.fillEllipse (cf);
                    juce::Path tick;
                    tick.startNewSubPath (cf.getX() + 7.5f, cf.getCentreY() + 0.5f);
                    tick.lineTo (cf.getCentreX() - 1.0f, cf.getBottom() - 8.5f);
                    tick.lineTo (cf.getRight() - 7.0f, cf.getY() + 8.5f);
                    g.setColour (t.accent());
                    g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                case DownloadManager::State::Downloading:
                case DownloadManager::State::Queued:
                {
                    g.setColour (t.textDim().withAlpha (0.3f));
                    g.drawEllipse (cf.reduced (3.0f), 2.5f);
                    const float p = juce::jmax (0.04f, ctx.downloads().getProgress (tr));
                    juce::Path arc;
                    arc.addCentredArc (cf.getCentreX(), cf.getCentreY(), cf.getWidth() * 0.5f - 3.0f, cf.getHeight() * 0.5f - 3.0f, 0.0f,
                                       0.0f, p * juce::MathConstants<float>::twoPi, true);
                    g.setColour (t.accent());
                    g.strokePath (arc, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                case DownloadManager::State::Failed:
                case DownloadManager::State::None:
                default:
                    if (i == hover || dlState == DownloadManager::State::Failed)
                        style::drawIcon (g, style::Icon::Download, cf.reduced (4.0f),
                                         dlState == DownloadManager::State::Failed ? juce::Colour (0xffff5c6c) : t.textCol().withAlpha (0.85f));
                    downloadButtons[(size_t) i] = cell;
                    break;
            }
        }
        if (tr.source.startsWith ("upnp"))
        {
            // NAS / Jellyfin badge next to the format badge
            const auto label = ctx.networkSourceLabel (tr.source).toUpperCase();
            auto b = r.removeFromRight (label.length() * 8 + 34).withSizeKeepingCentre (label.length() * 8 + 26, 18).toFloat();
            g.setColour (t.accent2().withAlpha (0.16f));
            g.fillRoundedRectangle (b, b.getHeight() * 0.35f);
            style::drawIcon (g, style::Icon::Network, b.removeFromLeft (18.0f).reduced (3.0f), t.accent2(), 2.0f);
            g.setColour (t.accent2());
            g.setFont (style::font (11.0f, true));
            g.drawText (label, b.withTrimmedRight (4.0f), juce::Justification::centred);
        }
        else if (tr.source != "local" || ctx.library().isOfflineCopy (tr))
        {
            auto s = r.removeFromRight (90);
            g.setColour ((tr.source == "local" ? t.accent() : t.textDim()).withAlpha (0.85f));
            g.setFont (style::font (11.5f, tr.source == "local"));
            const auto src = tr.source == "local" ? juce::String ("Hors-ligne") : tr.source.substring (0, 1).toUpperCase() + tr.source.substring (1);
            g.drawText (src, s, juce::Justification::centredRight);
        }
        juce::Rectangle<int> albumCol;
        if (album && r.getWidth() > 420) albumCol = r.removeFromRight (r.getWidth() / 3);

        g.setColour (isPlaying ? t.accent() : t.textCol());
        g.setFont (style::font (14.0f, true));
        g.drawText (tr.title, r.removeFromTop (r.getHeight() / 2).withTrimmedRight (12), juce::Justification::bottomLeft, true);
        {
            // artist name: accent colour, hand cursor, opens the artist page
            g.setColour (t.accent());
            g.setFont (style::font (13.0f));
            if ((int) artistRects.size() != (int) tracks.size()) artistRects.assign (tracks.size(), {});
            const auto area = r.withTrimmedRight (12);
            const auto artist = tr.displayArtist();
            const int aw = juce::jmin (area.getWidth(), juce::roundToInt (style::textWidth (style::font (13.0f), artist)) + 2);
            auto ar = area.withWidth (aw).withHeight (juce::jmin (area.getHeight(), 20));
            artistRects[(size_t) i] = artist.isNotEmpty() ? ar : juce::Rectangle<int>();
            g.drawText (artist, ar, juce::Justification::topLeft, true);
            if (i == hover && getMouseXYRelative().y / rowHeight == i && ar.contains (getMouseXYRelative()))
                g.fillRect (ar.getX(), ar.getY() + 17, aw, 1);
        }
        g.setColour (t.textDim());
        g.setFont (style::font (13.0f));
        if (! albumCol.isEmpty())
            g.drawText (tr.album, albumCol, juce::Justification::centredLeft, true);
    }
}

//==============================================================================
ScrollPage::ScrollPage (AppContext& c) : ctx (c)
{
    viewport.setViewedComponent (&contentComp, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
}

void ScrollPage::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutContent (getWidth() - viewport.getScrollBarThickness());
}

void ScrollPage::setContentHeight (int h)
{
    contentComp.setSize (getWidth() - viewport.getScrollBarThickness(), juce::jmax (h, getHeight()));
}

void drawPageTitle (juce::Graphics& g, const Theme& t, juce::Rectangle<int> r, const juce::String& title, const juce::String& subtitle)
{
    g.setColour (t.textCol());
    g.setFont (style::font (32.0f, true));
    g.drawText (title, r.removeFromTop (44), juce::Justification::bottomLeft, true);
    if (subtitle.isNotEmpty())
    {
        g.setColour (t.textDim());
        g.setFont (style::font (14.0f));
        g.drawText (subtitle, r.removeFromTop (24), juce::Justification::centredLeft, true);
    }
}

} // namespace nx

namespace nx
{

//==============================================================================
// "+" explanations (AURA DSP, Settings › Audio)
//==============================================================================
juce::StringArray explainerIds() { return { "e9ds", "dxd", "crossfeed", "headroom", "truepeak", "smart", "hpeq", "rate", "fades", "pcmdsd", "keepdsd" }; }

Explainer explainer (const juce::String& id)
{
    if (id == "e9ds")
        return { "E9DS",
                 "Baisse automatiquement les sibilances (« s », « ch ») ou les aigus durs autour de la fréquence choisie, seulement quand ils ressortent trop. Le reste du spectre ne bouge pas."_t,
                 "Masters modernes très brillants et voix proches du micro moins fatigants, surtout au casque ou sur un tweeter vif."_t,
                 "Tant qu'il est actif, plus de bit-perfect ; une source DSD passe par le DXD 64 bits. Sous le seuil, la sortie reste identique à l'entrée au bit près."_t };
    if (id == "dxd")
        return { "Conversion de fréquence (DXD)"_t,
                 "Suréchantillonne le PCM vers 352,8 / 384 kHz avec un filtre sinc polyphase 64 bits, avant le DAC."_t,
                 "Utile avec un DAC dont le filtre interne est moyen à 44,1 / 48 kHz : le filtrage se fait ici, plus raide et plus propre. Sans effet sur un DAC qui suréchantillonne déjà très bien."_t,
                 "Plus de bit-perfect, charge CPU un peu plus haute ; le DAC change de fréquence moins souvent (toujours 352,8 ou 384 kHz)."_t };
    if (id == "crossfeed")
        return { "Crossfeed",
                 "Mélange un peu de chaque voie dans l'autre, filtré et retardé comme à l'écoute sur enceintes (Bauer bs2b 700 Hz / 4,5 dB)."_t,
                 "Au casque : moins de fatigue sur les vieux mixages très séparés (jazz, rock des années 60), image recentrée devant soi."_t,
                 "Inutile sur enceintes ; stéréo un peu moins large ; plus de bit-perfect tant qu'il est actif."_t };
    if (id == "headroom")
        return { "Gestion du headroom"_t,
                 "Baisse le niveau juste ce qu'il faut avant les égaliseurs pour que leurs bosses ne dépassent jamais 0 dBFS."_t,
                 "Aucune saturation numérique quand un EQ remonte des basses ou des aigus, même sur des masters déjà au maximum."_t,
                 "Le volume baisse de la valeur de la plus grande bosse : à compenser au volume de l'ampli. Sans effet si aucun EQ n'est actif."_t };
    if (id == "truepeak")
        return { "Auto-Headroom True Peak"_t,
                 "Mesure en tâche de fond le vrai pic de chaque morceau entre les échantillons (True Peak, ITU-R BS.1770-4, suréchantillonné ×4). Si ce pic dépasse le plafond (-1,0 ou -0,5 dBTP), le morceau est baissé de l'écart exact, avant toute conversion, égaliseur ou modulateur DSD."_t,
                 "Plus de saturation sur les masters modernes qui frôlent 0 dBFS mais dépassent 0 dBTP : reconstruction du DAC, suréchantillonnage DXD et modulation DSD restent propres. Un morceau déjà sous le plafond garde 0 dB : sa dynamique et le bit-perfect sont intacts."_t,
                 "Un morceau baissé n'est plus bit-perfect (gain 64 bits + dither). À la première écoute, l'atténuation arrive dès la fin de la mesure (glissement de 10 ms) ; ensuite elle est connue dès le premier échantillon. Source DSD : True Peak affiché seulement. Flux réseau non mesurés."_t };
    if (id == "smart")
        return { "Smart Ear Comfort",
                 "Lit le profil de mastering de chaque morceau mesuré en tâche de fond (dynamique DR, part d'énergie entre 6 et 10 kHz). Sur un master brillant et compressé (DR 8 ou moins, aigus forts), E9DS s'active tout seul en Dynamic HF 8 kHz, dosé de -1,5 à -6 dB selon le morceau ; sur les autres, rien."_t,
                 "Une playlist variée s'écoute sans retoucher le DSP : moins de fatigue sur les masters modernes durs, et les enregistrements dynamiques passent intacts, en bit-perfect s'il n'y a pas d'autre traitement."_t,
                 "Un morceau traité n'est plus bit-perfect. E9DS réglé à la main reste prioritaire. Source DSD : profil affiché seulement. À la première écoute, l'adaptation arrive à la fin de la mesure (rampe de 20 ms) ; des cymbales très présentes peuvent compter comme des aigus forts, d'où la condition sur le DR."_t };
    if (id == "hpeq")
        return { "Égaliseur casque"_t,
                 "Corrige la courbe de réponse d'un casque mesuré pour la rapprocher d'une cible neutre (profils intégrés, 64 bits)."_t,
                 "Casques colorés (basses gonflées, pic dans les aigus) : timbres plus justes, voix plus naturelles."_t,
                 "Valable pour le modèle choisi seulement ; plus de bit-perfect ; le headroom baisse un peu le niveau."_t };
    if (id == "rate")
        return { "Fréquence de sortie"_t,
                 "Auto suit la fréquence du morceau, sauf en sortie sans fil (Bluetooth, AirPlay) où la fréquence réglée par le système est gardée. « Toujours suivre la source » et « Fixe » forcent l'un ou l'autre."_t,
                 "Enceinte ou casque Bluetooth : plus de coupures ni de pops entre deux morceaux de fréquences différentes."_t,
                 "Fréquence fixe : les morceaux d'une autre fréquence sont convertis (sinc 64 bits), donc plus de bit-perfect pour eux."_t };
    if (id == "fades")
        return { "Micro-fondus (10 ms)"_t,
                 "Ajoute un fondu de 10 ms au démarrage, à la pause, à l'arrêt, au saut de piste, au déplacement et au changement de fréquence."_t,
                 "Plus de « pop » ni de clic aux coupures, surtout audible au casque et sur les DAC qui se recalent."_t,
                 "Jamais entre deux morceaux enchaînés sans blanc (gapless intact). Désactivé : bit-perfect strict dès le premier échantillon."_t };
    if (id == "pcmdsd")
        return { "Conversion PCM -> DSD · XZ Automatic"_t,
                 "Transforme le PCM en DSD 1 bit (modulateur sigma-delta 64 bits, ordre 5 ou 7). XZ Automatic choisit le taux selon la source : 44,1 kHz -> DSD64 ; 48 / 88,2 / 176,4 kHz -> DSD128 ; 96 kHz et plus -> DSD256."_t,
                 "Avec un DAC qui sonne mieux en DSD Direct (chemin 1 bit sans filtre PCM) : présentation souvent plus fluide et aérée."_t,
                 "Plus de bit-perfect ; CPU d'un cœur : ~12 % en DSD64, ~21 % en DSD128, ~39 % en DSD256. Pas de gapless entre deux taux DSD différents (fondu de 10 ms). Repli automatique au taux maximum du DAC."_t };
    if (id == "keepdsd")
        return { "Garder la sortie DSD"_t,
                 "Une source DSD qui doit être traitée (AURA DSP, mixage 5.1 -> 2.0) passe en DXD 64 bits puis repasse en DSD au lieu de sortir en PCM."_t,
                 "Le DAC reste sur son chemin DSD : utile si vous le préférez en DSD Direct."_t,
                 "Le DSD d'origine n'est plus transmis tel quel (remodulé) ; charge CPU du modulateur ; un réglage d'EQ s'entend avec ~0,5 s de délai."_t };
    return {};
}

namespace
{
    juce::Component::SafePointer<juce::Component> openPopover;

    /** The popover: title, then the three points, in the theme's colours. */
    struct ExplainerView : juce::Component
    {
        ExplainerView (AppContext& c, Explainer e) : ctx (c), text (std::move (e))
        {
            setWantsKeyboardFocus (true);
            setSize (width, (int) std::ceil (layout().getHeight()) + 2 * pad);
        }
        juce::TextLayout layout() const
        {
            const auto& t = ctx.theme();
            juce::AttributedString as;
            as.setWordWrap (juce::AttributedString::byWord);
            as.setLineSpacing (1.5f);
            as.append (text.title + "\n", style::font (15.0f, true), t.textCol());
            auto point = [&] (const juce::String& head, const juce::String& body)
            {
                as.append ("\n" + head + "\n", style::font (12.0f, true), t.accent());
                as.append (body + "\n", style::font (13.0f), t.textCol().withAlpha (0.88f));
            };
            point ("CE QUE ÇA FAIT"_t, text.what);
            point ("GAIN RÉEL À L'ÉCOUTE"_t, text.gain);
            point ("COMPROMIS / LIMITES"_t, text.limits);
            juce::TextLayout tl;
            tl.createLayout (as, (float) (width - 2 * pad));
            return tl;
        }
        void paint (juce::Graphics& g) override { layout().draw (g, getLocalBounds().reduced (pad).toFloat()); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k != juce::KeyPress::escapeKey) return false;
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            return true;
        }
        static constexpr int width = 340, pad = 14;
        AppContext& ctx;
        Explainer text;
    };
}

InfoButton::InfoButton (AppContext& c, juce::String i) : juce::Button ("+"), id (std::move (i)), ctx (c)
{
    setTooltip ("En savoir plus"_t);
    setTitle ("En savoir plus : "_t + explainer (id).title);
    setWantsKeyboardFocus (false);
    setSize (size, size);
}

void InfoButton::paintButton (juce::Graphics& g, bool hi, bool down)
{
    const auto& t = ctx.theme();
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    const auto c = hi || down ? t.accent() : t.textDim();
    if (hi || down) { g.setColour (t.accent().withAlpha (0.15f)); g.fillEllipse (r); }
    g.setColour (c);
    g.drawEllipse (r, 1.2f);
    const auto m = r.getCentre();
    const float a = r.getWidth() * 0.24f;
    g.drawLine (m.x - a, m.y, m.x + a, m.y, 1.4f);
    g.drawLine (m.x, m.y - a, m.x, m.y + a, 1.4f);
}

void InfoButton::clicked()
{
    auto view = std::make_unique<ExplainerView> (ctx, explainer (id));
    openPopover = view.get();
    auto* top = getTopLevelComponent();
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (view), top->getLocalArea (this, getLocalBounds()), top);
    box.setArrowSize (8.0f);
    if (openPopover != nullptr) openPopover->grabKeyboardFocus();
}

juce::Component* InfoButton::lastPopover() { return openPopover.getComponent(); }

void syncHeart (IconButton& heart, const TrackInfo* track)
{
    const bool fav = track != nullptr && track->favourite;
    heart.setVisible (track != nullptr);
    heart.setIcon (fav ? style::Icon::Heart : style::Icon::HeartOutline);
    if (heart.active != fav) { heart.active = fav; heart.repaint(); }
}

} // namespace nx
