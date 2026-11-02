// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Lyrics.h"
#include "Library.h"
#include "TagReader.h"
#include <algorithm>

namespace nx
{

int Lyrics::indexAt (double t) const noexcept
{
    if (! synced || lines.empty()) return -1;
    auto it = std::upper_bound (lines.begin(), lines.end(), t, [] (double v, const LyricLine& l) { return v < l.time; });
    return (int) (it - lines.begin()) - 1;
}

namespace
{
    /** "mm:ss.xx" / "mm:ss:xx" / "mm:ss" -> seconds, or -1. */
    double parseTimeTag (const juce::String& tag)
    {
        const auto mm = tag.upToFirstOccurrenceOf (":", false, false);
        auto rest = tag.fromFirstOccurrenceOf (":", false, false);
        if (mm.isEmpty() || ! mm.containsOnly ("0123456789") || rest.isEmpty()) return -1.0;
        rest = rest.replaceCharacter (':', '.');
        if (! rest.containsOnly ("0123456789.")) return -1.0;
        return mm.getIntValue() * 60.0 + rest.getDoubleValue();
    }

    juce::String stripWordTags (const juce::String& s)
    {
        juce::String out;
        int i = 0;
        while (i < s.length())
        {
            if (s[i] == '<')
            {
                const int close = s.indexOfChar (i, '>');
                if (close > i && parseTimeTag (s.substring (i + 1, close)) >= 0.0) { i = close + 1; continue; }
            }
            out << s[i];
            ++i;
        }
        return out.trim();
    }
}

Lyrics parseLrc (const juce::String& text)
{
    Lyrics l;
    double offset = 0.0;
    juce::StringArray plainLines;

    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        std::vector<double> times;
        while (line.startsWithChar ('['))
        {
            const int close = line.indexOfChar (']');
            if (close < 0) break;
            const auto tag = line.substring (1, close);
            const double t = parseTimeTag (tag);
            if (t >= 0.0) times.push_back (t);
            else if (tag.startsWithIgnoreCase ("offset:")) offset = tag.fromFirstOccurrenceOf (":", false, false).trim().getDoubleValue() / 1000.0;
            else if (times.empty()) { line = {}; break; }   // [ar:], [ti:]... metadata line
            line = line.substring (close + 1).trimStart();
        }
        const auto words = stripWordTags (line);
        if (times.empty()) { if (words.isNotEmpty()) plainLines.add (words); continue; }
        for (auto t : times) l.lines.push_back ({ juce::jmax (0.0, t - offset), words });
    }

    std::stable_sort (l.lines.begin(), l.lines.end(), [] (const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
    l.synced = ! l.lines.empty();
    l.plain = l.synced ? juce::String() : plainLines.joinIntoString ("\n");
    return l;
}

Lyrics loadLocalLyrics (const TrackInfo& track)
{
    // Local sources only (no network): sidecar .lrc / .txt next to the file, then the embedded lyrics tags.
    auto fromFile = [] (const juce::File& f)
    {
        auto l = parseLrc (f.loadFileAsString());
        l.source = f.hasFileExtension ("lrc") ? "Fichier .lrc"_t : "Fichier .txt"_t;
        return l;
    };
    if (track.lyricsPath.isNotEmpty())
        if (auto l = fromFile (juce::File (track.lyricsPath)); ! l.isEmpty()) return l;
    if (track.source != "local") return {};

    juce::File f (track.externalId);
    for (auto ext : { "lrc", "txt" })
        if (auto side = f.withFileExtension (ext); side.existsAsFile())
            if (auto l = fromFile (side); ! l.isEmpty()) return l;

    TagData tags;
    if (! readTags (f, tags)) return {};
    for (auto key : { "syncedlyrics", "lyrics" })            // ID3 SYLT first (synced), then USLT / Vorbis LYRICS / MP4 ©lyr
        if (tags.get (key).isNotEmpty())
        {
            auto l = parseLrc (tags.get (key));
            l.source = "Tags";
            return l;
        }
    return {};
}

} // namespace nx
