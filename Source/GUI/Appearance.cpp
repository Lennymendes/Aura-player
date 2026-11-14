// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Appearance.h"

namespace nx::appearance
{

const std::array<Accent, 8>& accents()
{
    static const std::array<Accent, 8> list {{
        { "Thème", 0 }, { "Émeraude NX", signature }, { "Cyan", 0xff00d2ff }, { "Bleu Aura", 0xff2563eb },
        { "Violet", 0xff8b5cf6 }, { "Rose", 0xfffa2d55 }, { "Ambre", 0xfff59e0b }, { "Argent", 0xffe2e8f0 },
    }};
    return list;
}

namespace
{
    void frame (juce::Graphics& g, const Theme& t, juce::Rectangle<float> r, bool on, bool hot)
    {
        g.setColour (on ? t.accent().withAlpha (0.14f) : style::neutral().withAlpha (hot ? 0.08f : 0.04f));
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (on ? t.accent() : t.border());
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, on ? 2.0f : 1.0f);
    }

    /** A tiny window: sidebar, cards, player bar, in the given colours. */
    void miniWindow (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour bg, juce::Colour panel, juce::Colour ink, juce::Colour accent)
    {
        g.setColour (bg);
        g.fillRoundedRectangle (r, 7.0f);
        auto in = r.reduced (5.0f);
        auto bar = in.removeFromBottom (in.getHeight() * 0.2f);
        in.removeFromBottom (3.0f);
        auto side = in.removeFromLeft (in.getWidth() * 0.26f);
        in.removeFromLeft (3.0f);
        g.setColour (panel);
        g.fillRoundedRectangle (side, 4.0f);
        g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (side.getX() + 3.0f, side.getY() + 6.0f, side.getWidth() * 0.6f, 3.0f, 1.5f);
        g.fillRoundedRectangle (bar.withSizeKeepingCentre (bar.getWidth() * 0.4f, 2.5f), 1.2f);
        for (int k = 0; k < 3; ++k)
        {
            g.setColour (ink.withAlpha (0.18f + 0.08f * (float) k));
            g.fillRoundedRectangle (in.getX() + (float) k * in.getWidth() / 3.0f, in.getY(), in.getWidth() / 3.0f - 3.0f, in.getHeight() * 0.6f, 3.0f);
        }
    }
}

void paintModes (juce::Graphics& g, AppContext& ctx, juce::Rectangle<int> area, juce::Point<int> mouse,
                 std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit)
{
    const auto& t = ctx.theme();
    const auto mode = ctx.getAppearance();
    const juce::String ids[] = { "dark", "light", "system" };
    const juce::String names[] = { "Sombre"_t, "Clair"_t, "Système"_t };
    const auto accent = t.accent();
    const int w = (area.getWidth() - 24) / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto tile = area.removeFromLeft (w);
        area.removeFromLeft (12);
        frame (g, t, tile.toFloat(), mode == ids[i], tile.contains (mouse));
        auto pv = tile.reduced (12).removeFromTop (tile.getHeight() - 52).toFloat();
        const juce::Colour darkBg (0xff0a0c0e), darkPanel (0xff1a1f24), lightBg (0xfff4f5f7), lightPanel (0xffffffff);
        if (i == 2)
        {
            // half dark, half light
            {
                juce::Graphics::ScopedSaveState s (g);
                g.reduceClipRegion (pv.withWidth (pv.getWidth() * 0.5f).toNearestInt());
                miniWindow (g, pv, darkBg, darkPanel, juce::Colours::white, accent);
            }
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (pv.withTrimmedLeft (pv.getWidth() * 0.5f).toNearestInt());
            miniWindow (g, pv, lightBg, lightPanel, juce::Colour (0xff111827), accent.darker (0.3f));
        }
        else if (i == 0) miniWindow (g, pv, darkBg, darkPanel, juce::Colours::white, accent);
        else miniWindow (g, pv, lightBg, lightPanel, juce::Colour (0xff111827), accent.darker (0.3f));
        g.setColour (t.textCol());
        g.setFont (style::font (14.5f, true));
        g.drawText (names[i], tile.reduced (14, 10).removeFromBottom (24), juce::Justification::centredLeft);
        hit.push_back ({ ids[i], tile });
    }
}

void paintAccents (juce::Graphics& g, AppContext& ctx, juce::Rectangle<int> area, juce::Point<int> mouse,
                   std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit)
{
    const auto& t = ctx.theme();
    const auto chosen = ctx.getAccent();
    const int d = juce::jmin (30, area.getHeight());
    auto row = area.withHeight (d);
    juce::String hover;
    for (int i = 0; i < (int) accents().size(); ++i)
    {
        const auto& a = accents()[(size_t) i];
        auto r = row.removeFromLeft (d).toFloat();
        row.removeFromLeft (14);
        const bool on = a.argb == 0 ? chosen.isTransparent() : (! chosen.isTransparent() && chosen.getARGB() == a.argb);
        const bool hot = r.contains (mouse.toFloat());
        if (hot) hover = juce::String::fromUTF8 (a.name);
        if (a.argb == 0)
        {
            // "the theme's own": half the theme accent, half its second accent
            juce::Path half;
            half.addPieSegment (r.reduced (3.0f), 0.0f, juce::MathConstants<float>::pi, 0.0f);
            g.setColour (t.accent2());
            g.fillEllipse (r.reduced (3.0f));
            g.setColour (ctx.themes().findTheme (ctx.selectedThemeId()) != nullptr ? ctx.themes().findTheme (ctx.selectedThemeId())->accent() : t.accent());
            g.fillPath (half);
        }
        else
        {
            g.setColour (juce::Colour (a.argb));
            g.fillEllipse (r.reduced (3.0f));
        }
        if (on || hot)
        {
            g.setColour (on ? t.textCol() : t.textDim());
            g.drawEllipse (r.reduced (0.5f), on ? 2.0f : 1.2f);
        }
        if (a.argb == signature)
        {
            g.setColour (t.textDim());
            g.setFont (style::font (9.0f, true));
            g.drawText ("NX", r.translated (0.0f, r.getHeight() - 2.0f).withHeight (12.0f), juce::Justification::centred);
        }
        hit.push_back ({ juce::String (i), r.toNearestInt() });
    }
    g.setColour (t.textDim());
    g.setFont (style::font (12.5f));
    const auto* cur = [&]() -> const Accent* { for (auto& a : accents()) if ((a.argb == 0 && chosen.isTransparent()) || (! chosen.isTransparent() && a.argb == chosen.getARGB())) return &a; return nullptr; }();
    g.drawText (hover.isNotEmpty() ? hover : cur != nullptr ? juce::String::fromUTF8 (cur->name) : juce::String(), row.withTrimmedLeft (4), juce::Justification::centredLeft, true);
}

void paintDensities (juce::Graphics& g, AppContext& ctx, juce::Rectangle<int> area, juce::Point<int> mouse,
                     std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit)
{
    const auto& t = ctx.theme();
    const auto cur = ctx.settings().get ("layout").getProperty ("density", "comfortable").toString();
    const juce::String ids[] = { "comfortable", "compact", "integrated" };
    const juce::String names[] = { "Confortable"_t, "Compact"_t, "Compact intégré"_t };
    const float barH[] = { 0.24f, 0.16f, 0.13f };
    const int w = (area.getWidth() - 24) / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto tile = area.removeFromLeft (w);
        area.removeFromLeft (12);
        frame (g, t, tile.toFloat(), cur == ids[i], tile.contains (mouse));
        auto pv = tile.reduced (12).removeFromTop (tile.getHeight() - 48).toFloat();
        g.setColour (t.bg().brighter (0.06f));
        g.fillRoundedRectangle (pv, 6.0f);
        auto bar = i == 2 ? pv.withTop (pv.getBottom() - pv.getHeight() * barH[i])
                          : pv.reduced (6.0f).withTop (pv.getBottom() - 6.0f - pv.getHeight() * barH[i]);
        g.setColour (t.surface().brighter (0.2f));
        g.fillRoundedRectangle (bar, i == 2 ? 0.0f : 5.0f);
        g.setColour (t.accent());
        g.fillRoundedRectangle (bar.withSizeKeepingCentre (bar.getWidth() * 0.35f, 2.5f), 1.2f);
        g.setColour (t.textDim().withAlpha (0.6f));
        g.fillRoundedRectangle (bar.reduced (6.0f, 0).withWidth (bar.getHeight() - 4.0f).withSizeKeepingCentre (bar.getHeight() * 0.6f, bar.getHeight() * 0.6f), 2.0f);
        g.setColour (t.textCol());
        g.setFont (style::font (14.0f, true));
        g.drawText (names[i], tile.reduced (14, 10).removeFromBottom (22), juce::Justification::centredLeft);
        hit.push_back ({ ids[i], tile });
    }
}

void apply (AppContext& ctx, const juce::String& kind, const juce::String& id)
{
    if (kind == "mode") ctx.setAppearance (id);
    else if (kind == "accent")
    {
        const int i = id.getIntValue();
        if (i >= 0 && i < (int) accents().size())
            ctx.setAccent (accents()[(size_t) i].argb == 0 ? juce::Colours::transparentBlack : juce::Colour (accents()[(size_t) i].argb));
    }
    else if (kind == "density") ctx.setLayoutOption ("density", id);
}

void Picker::paint (juce::Graphics& g)
{
    const auto mouse = getMouseXYRelative();
    auto r = getLocalBounds();
    const auto& t = ctx.theme();
    modeHits.clear(); accentHits.clear(); densityHits.clear();
    if (kind == Kind::Density)
    {
        paintDensities (g, ctx, r.removeFromTop (juce::jmin (r.getHeight(), 128)).withWidth (juce::jmin (r.getWidth(), 620)), mouse, densityHits);
        return;
    }
    paintModes (g, ctx, r.removeFromTop (140).withWidth (juce::jmin (r.getWidth(), 620)), mouse, modeHits);
    r.removeFromTop (14);
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText ("COULEUR D'ACCENT"_t, r.removeFromTop (18), juce::Justification::centredLeft);
    r.removeFromTop (6);
    paintAccents (g, ctx, r.removeFromTop (34), mouse, accentHits);
}

void Picker::mouseUp (const juce::MouseEvent& e)
{
    for (auto* list : { &modeHits, &accentHits, &densityHits })
        for (auto& [id, b] : *list)
            if (b.contains (e.getPosition()))
            {
                apply (ctx, list == &modeHits ? "mode" : list == &accentHits ? "accent" : "density", id);
                repaint();
                return;
            }
}

} // namespace nx::appearance
