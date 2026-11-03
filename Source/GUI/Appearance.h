// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Appearance (Cider 2 style), shared by the first-run assistant and Réglages › Interface:
//   theme mode  Sombre / Clair / Système (follows the OS)
//   accent      a row of colour dots; NX Emerald #00F5A0 is the signature accent of NX Audio
// plus the listening-bar density tiles (Confortable / Compact / Compact intégré).
#pragma once

#include "Widgets.h"
#include <array>

namespace nx::appearance
{

struct Accent { const char* name; juce::uint32 argb; };   // argb 0 = the theme's own accent
const std::array<Accent, 8>& accents();
constexpr juce::uint32 signature = 0xff00f5a0;             // NX Emerald

/** Paints the three mode tiles; `hit` receives { id, bounds } for each. */
void paintModes (juce::Graphics&, AppContext&, juce::Rectangle<int> area, juce::Point<int> mouse,
                 std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit);
/** Paints the accent dots (id = index in accents()). */
void paintAccents (juce::Graphics&, AppContext&, juce::Rectangle<int> area, juce::Point<int> mouse,
                   std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit);
/** Paints the density tiles (id = comfortable / compact / integrated). */
void paintDensities (juce::Graphics&, AppContext&, juce::Rectangle<int> area, juce::Point<int> mouse,
                     std::vector<std::pair<juce::String, juce::Rectangle<int>>>& hit);
/** Applies a click: kind = "mode", "accent" or "density". */
void apply (AppContext&, const juce::String& kind, const juce::String& id);

/** Réglages › Interface: modes + accent dots, or the density tiles. */
class Picker : public juce::Component
{
public:
    enum class Kind { Appearance, Density };
    Picker (AppContext& c, Kind k) : ctx (c), kind (k) { setRepaintsOnMouseActivity (true); }
    static int idealHeight (Kind k) { return k == Kind::Appearance ? 214 : 132; }
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> modeHits, accentHits, densityHits;
private:
    AppContext& ctx;
    Kind kind;
};

} // namespace nx::appearance
