// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "LibraryDoctor.h"
#include <juce_graphics/juce_graphics.h>
#include <algorithm>
#include <map>

namespace nx::doctor
{

double qualityScore (const TrackInfo& t, int dr)
{
    if (t.lossy)
        return t.duration > 0.0 ? (double) t.fileSize * 8.0 / t.duration / 1000.0 : 0.0;     // kb/s, always < 1e6
    const double bits = t.isDsd ? 24.0 : (double) juce::jmax (1, t.bitsPerSample);
    const double rate = (t.isDsd ? t.sampleRate / 32.0 : t.sampleRate) / 1000.0;
    return 1.0e6 + bits * 1000.0 + rate + juce::jlimit (0, 30, dr) * 0.01;
}

juce::String qualityLabel (const TrackInfo& t, int dr)
{
    juce::String s = t.codec.isNotEmpty() ? t.codec : juce::String ("?");
    if (t.lossy) { if (t.duration > 0.0) s << " " << juce::roundToInt ((double) t.fileSize * 8.0 / t.duration / 1000.0) << " kb/s"; }
    else s << " " << t.resolutionBadge();
    if (dr > 0) s << juce::String::fromUTF8 (" \xc2\xb7 ") << "DR" << dr;
    return s;
}

juce::String normalise (const juce::String& in)
{
    auto s = in.toLowerCase();
    // drop a trailing "(...)" / "[...]": remaster, version, edition notes
    for (int guard = 0; guard < 3; ++guard)
    {
        const auto t = s.trimEnd();
        if (! (t.endsWithChar (')') || t.endsWithChar (']'))) break;
        const int open = juce::jmax (t.lastIndexOfChar ('('), t.lastIndexOfChar ('['));
        if (open <= 0) break;
        s = t.substring (0, open);
    }
    juce::String out;
    for (auto c : s) if (juce::CharacterFunctions::isLetterOrDigit (c)) out << juce::String::charToString (c);
    return out;
}

std::vector<DuplicateGroup> findDuplicates (Library& lib)
{
    std::map<juce::String, std::vector<TrackInfo>> byKey;
    for (auto& t : lib.getTracks ("artist", 1000000))
        if (t.source == "local" && t.title.isNotEmpty())
            byKey[normalise (t.displayArtist()) + "|" + normalise (t.title)].push_back (t);

    std::vector<DuplicateGroup> out;
    for (auto& [key, list] : byKey)
    {
        if (list.size() < 2) continue;
        std::vector<bool> used (list.size(), false);
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (used[i]) continue;
            DuplicateGroup g;
            g.artist = list[i].displayArtist();
            g.title = list[i].title;
            for (size_t j = i; j < list.size(); ++j)
            {
                if (used[j] || (j != i && list[j].externalId == list[i].externalId)) continue;
                const bool close = list[i].duration <= 0.0 || list[j].duration <= 0.0 || std::abs (list[i].duration - list[j].duration) <= 3.0;
                if (! close) continue;
                used[j] = true;
                const auto p = lib.getProfile (list[j].externalId);
                const int dr = p ? p->first : 0;
                g.versions.push_back ({ list[j], dr, qualityScore (list[j], dr), qualityLabel (list[j], dr) });
            }
            if (g.versions.size() < 2) continue;
            std::stable_sort (g.versions.begin(), g.versions.end(), [] (const Version& a, const Version& b) { return a.score > b.score; });
            out.push_back (std::move (g));
        }
    }
    std::sort (out.begin(), out.end(), [] (const DuplicateGroup& a, const DuplicateGroup& b)
               { return a.artist.compareNatural (b.artist) < 0 || (a.artist == b.artist && a.title.compareNatural (b.title) < 0); });
    return out;
}

bool moveToTrash (Library& lib, const TrackInfo& t)
{
    const juce::File f (t.externalId);
    if (t.source != "local" || ! f.existsAsFile()) return false;
    if (! f.moveToTrash()) return false;
    lib.remove (t.source, t.externalId);
    return true;
}

Audit audit (Library& lib)
{
    Audit a;
    std::map<juce::String, juce::Point<int>> sizes;      // artwork key -> pixels
    for (auto& al : lib.getAlbums ("artist", 100000))
    {
        if (al.fromNas() || al.source != "local") continue;
        if (al.artworkKey.isEmpty()) { a.missingCovers.push_back ({ al }); continue; }
        if (al.artworkKey.startsWith ("http")) continue;
        auto it = sizes.find (al.artworkKey);
        if (it == sizes.end())
        {
            const auto img = juce::ImageFileFormat::loadFrom (lib.getArtworkFile (al.artworkKey));
            it = sizes.emplace (al.artworkKey, juce::Point<int> (img.getWidth(), img.getHeight())).first;
        }
        if (it->second.x > 0 && (it->second.x < minCoverSize || it->second.y < minCoverSize))
            a.lowResCovers.push_back ({ al, it->second.x, it->second.y });
    }

    // tags, and albums of one folder split between album artists / years
    struct Folder { std::map<juce::String, int> artists; std::map<int, int> years; };
    std::map<juce::String, Folder> folders;            // album (lower case) + folder
    std::map<juce::String, juce::String> albumName, folderName;
    for (auto& t : lib.getTracks ("artist", 1000000))
    {
        if (t.source != "local") continue;
        TagIssue issue { t, {} };
        if (t.displayArtist().trim().isEmpty()) issue.missing.add ("artiste"_t);
        if (t.album.trim().isEmpty()) issue.missing.add ("album"_t);
        if (t.trackNumber <= 0) issue.missing.add ("numéro"_t);
        if (t.year <= 0) issue.missing.add ("année"_t);
        if (! issue.missing.isEmpty()) a.incompleteTags.push_back (issue);
        if (t.album.trim().isEmpty()) continue;
        const auto folder = juce::File (t.externalId).getParentDirectory().getFullPathName();
        const auto key = t.album.toLowerCase() + "\x1f" + folder;
        auto& f = folders[key];
        ++f.artists[t.albumArtist.isNotEmpty() ? t.albumArtist : t.artist];
        if (t.year > 0) ++f.years[t.year];
        albumName[key] = t.album;
        folderName[key] = folder;
    }
    for (auto& [key, f] : folders)
    {
        if (f.artists.size() < 2 && f.years.size() < 2) continue;
        AlbumFix fix;
        fix.album = albumName[key];
        fix.folder = folderName[key];
        int best = -1;
        for (auto& [artist, n] : f.artists) if (n > best) { best = n; fix.artist = artist; }
        best = -1;
        for (auto& [year, n] : f.years) if (n > best) { best = n; fix.year = year; }
        for (auto& [artist, n] : f.artists) if (artist != fix.artist) fix.otherArtists.add (artist);
        fix.years = f.years.size() > 1;
        a.inconsistentAlbums.push_back (fix);
    }
    return a;
}

int makeUniform (Library& lib, const AlbumFix& fix)
{
    int n = 0;
    for (auto& k : fix.otherArtists) n += lib.updateAlbum (k, fix.album, fix.album, fix.artist, fix.year, {});
    if (fix.years) lib.updateAlbum (fix.artist, fix.album, fix.album, fix.artist, fix.year, {});
    return juce::jmax (n, (int) lib.getAlbumTracks (fix.artist, fix.album).size());   // the tracks of the uniform album
}

juce::URL itunesSearchUrl (const juce::String& artist, const juce::String& album)
{
    return juce::URL ("https://itunes.apple.com/search")
        .withParameter ("term", (artist + " " + album).trim())
        .withParameter ("entity", "album")
        .withParameter ("limit", "1");
}

juce::String itunesArtworkFromJson (const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    if (const auto* results = v.getProperty ("results", {}).getArray(); results != nullptr && ! results->isEmpty())
    {
        const auto url = (*results)[0].getProperty ("artworkUrl100", {}).toString();
        if (url.startsWith ("https://")) return url.replace ("100x100bb", "1200x1200bb");
    }
    return {};
}

} // namespace nx::doctor
