// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Official web addresses, in one place. Each can be overridden in settings.json ("purchaseUrl", "discordUrl"),
// which lets a build be pointed at the shop / community without recompiling.
#pragma once

#include <juce_core/juce_core.h>

namespace nx::links
{

/** NX Player Premium shop. shortcut: still the GitHub page until the shop URL is known — set it here (or "purchaseUrl"). */
constexpr const char* shop = "https://github.com/Lennymendes/NX-Player#nx-player-premium";
/** "Rejoindre la communauté NX Audio" invitation. Empty: every Discord button stays hidden. */
constexpr const char* discord = "";

/** The address to use: the settings override, else the built-in one. */
inline juce::String resolve (const juce::var& settingsValue, const char* builtIn)
{
    const auto s = settingsValue.toString().trim();
    return s.startsWith ("https://") ? s : juce::String (builtIn);
}

} // namespace nx::links
