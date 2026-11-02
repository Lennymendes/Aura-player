// NX Player — Copyright (c) 2026 Lenny Antunes Mendes (NX Audio). Proprietary, all rights reserved (see LICENSE).
// Time-synced lyrics: LRC parser (multiple time tags per line, [offset:], enhanced <mm:ss.xx> word tags
// stripped), local sources only (sidecar .lrc / .txt, embedded tags): no online lyrics service.
#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

namespace nx
{

struct TrackInfo;

struct LyricLine
{
    double time = 0.0;      // seconds
    juce::String text;
};

struct Lyrics
{
    bool synced = false;
    std::vector<LyricLine> lines;   // sorted by time when synced
    juce::String plain;
    juce::String source;            // "Fichier .lrc", "Fichier .txt", "Tags"

    bool isEmpty() const noexcept { return lines.empty() && plain.isEmpty(); }
    /** Index of the line being sung at time t (-1 before the first line). */
    int indexAt (double t) const noexcept;
};

Lyrics parseLrc (const juce::String& text);
Lyrics loadLocalLyrics (const TrackInfo& track);


} // namespace nx
