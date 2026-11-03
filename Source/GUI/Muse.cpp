// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Muse.h"
#include "../DSP/StudioTools.h"

namespace nx
{

MusePanel::MusePanel (AppContext& c) : ctx (c), closeButton (style::Icon::Close, "Fermer"_t, c)
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (closeButton);
    closeButton.onClick = [this] { close(); };
    for (auto* t : { &headroom, &upsample, &crossfeed, &eq, &phase }) addAndMakeVisible (t);
    headroom.onClick = [this]
    {
        auto& s = ctx.studioTools();
        s.autoHeadroom = headroom.getToggleState();
        ctx.setHeadphoneEq (s.eqPreset.load(), s.eqBypass.load());     // preamp worked out again
    };
    upsample.onClick = [this]
    {
        auto st = ctx.engine().getSettings();
        st.upsampleToDxd = upsample.getToggleState();
        ctx.engine().setSettings (st);
        ctx.applyEngineSettings();
    };
    crossfeed.onClick = [this] { ctx.studioTools().crossfeed = crossfeed.getToggleState(); ctx.applyStudioTools(); repaint(); };
    phase.onClick = [this] { ctx.studioTools().invertPhase = phase.getToggleState(); ctx.applyStudioTools(); repaint(); };
    eq.onClick = [this]
    {
        auto& s = ctx.studioTools();
        const int preset = s.eqPreset.load() >= 0 ? s.eqPreset.load() : 0;     // first switch-on: MOMENTUM M2
        ctx.setHeadphoneEq (preset, ! eq.getToggleState());
        repaint();
    };
    setVisible (false);
}

void MusePanel::open()
{
    syncToggles();
    setVisible (true);
    toFront (true);
    resized();
    startTimerHz (2);           // source / bit-perfect state while it is open
}

void MusePanel::close()
{
    stopTimer();
    setVisible (false);
}

void MusePanel::syncToggles()
{
    const auto& s = ctx.studioTools();
    headroom.setToggleState (s.autoHeadroom.load(), juce::dontSendNotification);
    upsample.setToggleState (ctx.engine().getSettings().upsampleToDxd, juce::dontSendNotification);
    crossfeed.setToggleState (s.crossfeed.load(), juce::dontSendNotification);
    eq.setToggleState (s.eqActive(), juce::dontSendNotification);
    phase.setToggleState (s.invertPhase.load(), juce::dontSendNotification);
}

void MusePanel::choosePreset (int index)
{
    ctx.setHeadphoneEq (index, false);
    syncToggles();
    repaint();
}

void MusePanel::chooseMonitor (int index)
{
    using M = StudioToolsState::Monitor;
    const M modes[] = { M::Stereo, M::Mono, M::MidSolo, M::SideSolo };
    ctx.studioTools().monitor = modes[juce::jlimit (0, 3, index)];
    ctx.applyStudioTools();
    repaint();
}

juce::Rectangle<int> MusePanel::panelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (980, getWidth() - 60), juce::jmin (660, getHeight() - 48));
}

namespace
{
    constexpr int rowH = 58;
    const char* const rowTitles[] = { "Source", "Gestion du headroom", "Conversion de fréquence", "Crossfeed", "Égaliseur casque", "Polarité" };
}

void MusePanel::resized()
{
    auto p = panelBounds().reduced (26, 22);
    closeButton.setBounds (p.removeFromTop (40).removeFromRight (36).withSizeKeepingCentre (32, 32));
    p.removeFromTop (14);
    auto left = p.removeFromLeft (juce::jmin (380, p.getWidth() / 2 - 10));
    sourceArea = left.removeFromTop (rowH);
    juce::ToggleButton* toggles[] = { &headroom, &upsample, &crossfeed, &eq, &phase };
    for (auto* t : toggles)
    {
        auto row = left.removeFromTop (rowH);
        t->setBounds (row.removeFromRight (52).withSizeKeepingCentre (48, 28));
    }
    p.removeFromLeft (24);
    graphArea = p;
}

void MusePanel::mouseUp (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.getPosition())) { close(); return; }
    for (auto& [r, i] : presetTiles) if (r.contains (e.getPosition())) { choosePreset (i); return; }
    for (auto& [r, i] : monitorSegs) if (r.contains (e.getPosition())) { chooseMonitor (i); return; }
}

void MusePanel::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    const auto mouse = getMouseXYRelative();
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    auto pf = panelBounds().toFloat();
    juce::Path shape;
    shape.addRoundedRectangle (pf, t.radius() + 4.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.7f), 40, { 0, 16 }).drawForPath (g, shape);
    style::paintGlass (g, *this, ctx, pf, t.radius() + 4.0f, true);

    auto p = panelBounds().reduced (26, 22);
    auto head = p.removeFromTop (40);
    g.setColour (t.textCol());
    g.setFont (style::font (26.0f, true));
    g.drawText ("MUSE", head.removeFromLeft (100), juce::Justification::centredLeft);
    g.setColour (t.textDim());
    g.setFont (style::font (13.5f));
    g.drawText ("Precision Audio Control · traitement 64 bits"_t, head, juce::Justification::centredLeft);
    p.removeFromTop (14);

    auto& s = ctx.studioTools();
    const auto path = ctx.engine().getSignalPath();
    const bool bitPerfect = ! path.nodes.empty() && path.overall == PathQuality::BitPerfect;
    auto left = p.removeFromLeft (juce::jmin (380, p.getWidth() / 2 - 10));

    // the chain, one row per stage, a line joining them (Roon MUSE style)
    auto drawRow = [&] (juce::Rectangle<int> row, int i, bool on, const juce::String& detail)
    {
        auto dot = juce::Rectangle<float> (10, 10).withCentre ({ (float) row.getX() + 8.0f, (float) row.getCentreY() });
        if (i > 0)
        {
            g.setColour (t.border());
            g.fillRect ((float) row.getX() + 7.5f, (float) row.getY() - (float) rowH / 2.0f + 6.0f, 1.0f, (float) rowH - 12.0f);
        }
        g.setColour (on ? t.accent() : t.textDim().withAlpha (0.5f));
        g.fillEllipse (dot);
        if (on) { g.setColour (t.accent().withAlpha (0.25f)); g.drawEllipse (dot.expanded (3.0f), 2.0f); }
        auto text = row.withTrimmedLeft (26).withTrimmedRight (60);
        g.setColour (t.textCol());
        g.setFont (style::font (15.0f, true));
        g.drawText (tr (juce::String::fromUTF8 (rowTitles[i])), text.removeFromTop (text.getHeight() / 2 + 2), juce::Justification::bottomLeft, true);
        g.setColour (t.textDim());
        g.setFont (style::font (12.5f));
        g.drawText (detail, text, juce::Justification::topLeft, true);
    };
    {
        const auto* tr0 = ctx.currentTrack();
        const juce::String src = tr0 != nullptr ? tr0->resolutionBadge() + "  ·  " + ctx.engine().getDeviceName() : juce::String ("Rien en lecture"_t);
        drawRow (sourceArea, 0, bitPerfect, src + (bitPerfect ? "  ·  Bit-perfect"_t : juce::String()));
    }
    const auto* preset = s.eqPresetInfo();
    const double preampDb = s.eqActive() ? s.eqPreampDb.load() : 0.0;
    auto rowOf = [&] (juce::ToggleButton& b) { return juce::Rectangle<int> (left.getX(), b.getBounds().getCentreY() - rowH / 2, left.getWidth(), rowH); };
    drawRow (rowOf (headroom), 1,
             s.eqActive() && s.autoHeadroom.load(),
             s.autoHeadroom.load() ? (s.eqActive() ? "Automatique : "_t + juce::String (preampDb, 1).replaceCharacter ('.', ',') + " dB, jamais d'écrêtage"_t : juce::String ("Automatique (aucun gain à compenser)"_t))
                                   : juce::String ("Désactivé : attention à l'écrêtage"_t));
    drawRow (rowOf (upsample), 2, upsample.getToggleState(), upsample.getToggleState() ? "Sinc polyphase vers DXD 352,8 / 384 kHz"_t : juce::String ("Fréquence d'origine (bit-perfect possible)"_t));
    drawRow (rowOf (crossfeed), 3, s.crossfeed.load(), "Bauer bs2b 700 Hz / 4,5 dB, pour le casque"_t);
    drawRow (rowOf (eq), 4, s.eqActive(), preset != nullptr ? juce::String::fromUTF8 (preset->name) + (s.eqBypass.load() ? "  ·  contourné"_t : juce::String()) : juce::String ("Choisissez un profil à droite"_t));
    drawRow (rowOf (phase), 5, s.invertPhase.load(), "Inversion de polarité absolue (Ø)"_t);

    // monitoring segments under the chain
    monitorSegs.clear();
    {
        auto seg = juce::Rectangle<int> (left.getX(), rowOf (phase).getBottom() + 14, left.getWidth(), 32);
        const juce::String names[] = { "Stéréo"_t, "Mono", "Mid", "Side" };
        const int cur = (int) s.monitor.load();
        const int w = seg.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            auto r = seg.removeFromLeft (w).reduced (2, 0);
            const bool on = i == cur;
            g.setColour (on ? t.accent() : style::neutral().withAlpha (r.contains (mouse) ? 0.12f : 0.06f));
            g.fillRoundedRectangle (r.toFloat(), 9.0f);
            g.setColour (on ? t.colour ("--nx-on-accent", juce::Colours::black) : t.textCol());
            g.setFont (style::font (13.0f, true));
            g.drawText (names[i], r, juce::Justification::centred);
            monitorSegs.push_back ({ r, i });
        }
    }

    // right: headphone presets + response curve
    p.removeFromLeft (24);
    auto right = p;
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText ("PROFILS CASQUE"_t, right.removeFromTop (20), juce::Justification::centredLeft);
    right.removeFromTop (6);
    presetTiles.clear();
    const auto& list = hpeq::presets();
    const int cols = 3, tileH = 62;
    const int tw = (right.getWidth() - 10 * (cols - 1)) / cols;
    auto grid = right.removeFromTop (((int) list.size() + cols - 1) / cols * (tileH + 10));
    for (int i = 0; i < (int) list.size(); ++i)
    {
        auto r = juce::Rectangle<int> (grid.getX() + (i % cols) * (tw + 10), grid.getY() + (i / cols) * (tileH + 10), tw, tileH);
        const bool on = s.eqPreset.load() == i;
        g.setColour (on ? t.accent().withAlpha (0.16f) : style::neutral().withAlpha (r.contains (mouse) ? 0.09f : 0.045f));
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        g.setColour (on ? t.accent() : t.border());
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 12.0f, on ? 1.6f : 1.0f);
        auto inner = r.reduced (12, 10);
        style::drawIcon (g, list[(size_t) i].overEar ? style::Icon::Headphones : style::Icon::Studio,
                         inner.removeFromLeft (28).toFloat().withSizeKeepingCentre (24, 24), on ? t.accent() : t.textDim());
        inner.removeFromLeft (8);
        g.setColour (t.textCol());
        g.setFont (style::font (13.0f, true));
        g.drawFittedText (juce::String::fromUTF8 (list[(size_t) i].name), inner.removeFromTop (inner.getHeight() / 2 + 1), juce::Justification::bottomLeft, 1, 0.75f);
        g.setColour (t.textDim());
        g.setFont (style::font (11.5f));
        const juce::String maker (list[(size_t) i].maker);
        g.drawText (maker.isNotEmpty() ? maker : juce::String ("Courbe générique"_t), inner, juce::Justification::topLeft, true);
        presetTiles.push_back ({ r, i });
    }
    right.removeFromTop (8);

    // frequency response, 20 Hz - 20 kHz, ±12 dB
    auto graph = right.removeFromTop (juce::jmax (120, right.getHeight() - 96)).toFloat();
    g.setColour (style::neutral().withAlpha (0.04f));
    g.fillRoundedRectangle (graph, 12.0f);
    auto plot = graph.reduced (36.0f, 14.0f);
    auto xOf = [&] (double f) { return plot.getX() + (float) (std::log10 (f / 20.0) / 3.0) * plot.getWidth(); };
    auto yOf = [&] (double db) { return plot.getCentreY() - (float) (db / 12.0) * plot.getHeight() * 0.5f; };
    g.setFont (style::font (10.5f));
    for (double f : { 20.0, 100.0, 1000.0, 10000.0, 20000.0 })
    {
        g.setColour (t.border());
        g.fillRect (xOf (f), plot.getY(), 1.0f, plot.getHeight());
        g.setColour (t.textDim());
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f), juce::Rectangle<float> (40, 14).withCentre ({ xOf (f), plot.getBottom() + 8.0f }), juce::Justification::centred);
    }
    for (int db : { -12, -6, 0, 6, 12 })
    {
        g.setColour (db == 0 ? t.textDim().withAlpha (0.6f) : t.border());
        g.fillRect (plot.getX(), yOf (db), plot.getWidth(), 1.0f);
        g.setColour (t.textDim());
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (30, 12).withPosition (graph.getX() + 4.0f, yOf (db) - 6.0f), juce::Justification::centredLeft);
    }
    if (preset != nullptr)
    {
        juce::Path curve;
        const double fs = 48000.0;
        for (int i = 0; i <= 160; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 160.0);
            const auto pt = juce::Point<float> (xOf (f), yOf (juce::jlimit (-12.0, 12.0, hpeq::responseDb (*preset, fs, f) + (s.autoHeadroom.load() ? preampDb : 0.0))));
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour ((s.eqActive() ? t.accent() : t.textDim()).withAlpha (0.9f));
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));
    }

    // band table of the chosen profile
    right.removeFromTop (10);
    if (preset != nullptr)
    {
        g.setFont (style::font (11.5f));
        const int n = preset->numBands, cw = right.getWidth() / juce::jmax (1, n);
        for (int b = 0; b < n; ++b)
        {
            const auto& band = preset->bands[(size_t) b];
            auto cell = right.removeFromLeft (cw).reduced (2, 0);
            const juce::String kind = band.type == hpeq::BandType::LowShelf ? "Bass" : band.type == hpeq::BandType::HighShelf ? "Treble" : "Band " + juce::String (b);
            g.setColour (t.textCol());
            g.drawText (kind + " " + (band.gain > 0 ? "+" : "") + juce::String (band.gain, 1), cell.removeFromTop (16), juce::Justification::centredLeft, true);
            g.setColour (t.textDim());
            g.drawText ((band.freq >= 1000.0 ? juce::String (band.freq / 1000.0, 1) + "k" : juce::String ((int) band.freq)) + " Hz  Q " + juce::String (band.q, 1),
                        cell.removeFromTop (16), juce::Justification::centredLeft, true);
        }
    }
}

} // namespace nx
