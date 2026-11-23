// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Tag & Cover Doctor (Extensions): library clean-up, never destructive on its own.
//   Duplicates  local tracks with the same artist + title (case, accents of punctuation and "(Remaster...)" ignored)
//               and a duration within 3 s, ranked by objective quality: lossless > lossy, then bit depth, sample rate
//               (DSD counts as 24 bits at rate / 32), then the measured DR; lossy versions by bitrate.
//               The lower versions can be hidden (library only, reversible) or moved to the system trash (after an
//               explicit confirmation in the panel).
//   Audit       albums without cover, covers under 500 x 500 px, tracks without artist / album / number / year, albums
//               split in several "album artists" or with different years in one folder (made uniform in the library:
//               the files and their tags are never written).
//   Covers      a local image, or (opt-in) the iTunes Search API's 1200 x 1200 artwork.
#pragma once

#include "Library.h"
#include <vector>

namespace nx::doctor
{

struct Version
{
    TrackInfo track;
    int dr = 0;
    double score = 0.0;
    juce::String label;                      // "FLAC 24/96 · DR12"
};

struct DuplicateGroup
{
    juce::String artist, title;
    std::vector<Version> versions;           // best first
};

/** Quality score (higher = better) and its label. */
double qualityScore (const TrackInfo& t, int dr);
juce::String qualityLabel (const TrackInfo& t, int dr);
/** "The Wall (2011 Remaster)" -> "thewall" */
juce::String normalise (const juce::String& s);
std::vector<DuplicateGroup> findDuplicates (Library& lib);

/** Moves one file to the system trash, then drops it from the library. False (and nothing removed) when the trash
    refused it. Only called after the user's confirmation. */
bool moveToTrash (Library& lib, const TrackInfo& t);

struct CoverIssue { AlbumInfo album; int width = 0, height = 0; };    // 0 x 0 = no cover
struct TagIssue { TrackInfo track; juce::StringArray missing; };
struct AlbumFix
{
    juce::String album, folder, artist;      // the majority album artist
    int year = 0;                            // the majority year (0 = none)
    juce::StringArray otherArtists;          // keys to merge into `artist`
    bool years = false;                      // several years in the folder
};
struct Audit
{
    std::vector<CoverIssue> missingCovers, lowResCovers;
    std::vector<TagIssue> incompleteTags;
    std::vector<AlbumFix> inconsistentAlbums;
};
static constexpr int minCoverSize = 500;
Audit audit (Library& lib);
/** Makes an album uniform in the library (one album artist, one year). Returns the number of tracks of the album. */
int makeUniform (Library& lib, const AlbumFix& fix);

/** iTunes Search API (public, no key): the query, and the 1200 x 1200 artwork URL of its first album (empty if none). */
juce::URL itunesSearchUrl (const juce::String& artist, const juce::String& album);
juce::String itunesArtworkFromJson (const juce::String& json);

} // namespace nx::doctor
