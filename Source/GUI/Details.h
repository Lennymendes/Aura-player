// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Detailed metadata of a track or an album: the library's data, what the file header says and every tag the file carries
// (ID3v2, Vorbis / FLAC, MP4, WAV INFO). Read-only: nothing is written to the files.
#pragma once
#include "../DSP/HiResCheck.h"
#include <limits>

#include "AppContext.h"
#include "Widgets.h"

namespace nx
{

struct DetailRow { juce::String label, value; };
struct DetailSection { juce::String title; std::vector<DetailRow> rows; };

/** One track: Morceau / Audio / Fichier / Étiquettes (read from the file when it is local and present) / Crédits. */
/** truePeakDb: measured True Peak (NaN = not measured); ceilingDb: Auto-Headroom ceiling (NaN = Auto-Headroom off);
    dr / hfShareDb: the analysis for the mastering profile (0 / NaN = not measured), smartComfort: Smart Ear Comfort on. */
std::vector<DetailSection> trackDetails (const TrackInfo& track, double truePeakDb = std::numeric_limits<double>::quiet_NaN(),
                                         double ceilingDb = std::numeric_limits<double>::quiet_NaN(),
                                         int dr = 0, double hfShareDb = std::numeric_limits<double>::quiet_NaN(), bool smartComfort = false,
                                         const hires::Check* hiRes = nullptr);
/** An album: summary (formats, duration, total size) then one line per track. */
std::vector<DetailSection> albumDetails (const std::vector<TrackInfo>& tracks, const juce::String& albumTitle);
/** Plain text, for the "Copier" button and bug reports. */
juce::String detailsToText (const juce::String& heading, const std::vector<DetailSection>& sections);

class DetailsSheet : public juce::Component
{
public:
    explicit DetailsSheet (AppContext& c);
    /** One track = its sheet; several = the album sheet (albumTitle), a click on a track opens its own sheet. */
    void open (const std::vector<TrackInfo>& tracks, const juce::String& albumTitle);
    void close();
    const std::vector<DetailSection>& getSections() const noexcept { return sections; }
    juce::String getText() const { return detailsToText (heading, sections); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override { if (! panel.contains (e.getPosition())) close(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }
    IconButton closeButton;
    juce::TextButton copyButton;
private:
    struct Body : juce::Component
    {
        explicit Body (DetailsSheet& s) : sheet (s) {}
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent& e) override;
        int layoutFor (int width);
        DetailsSheet& sheet;
        struct Line { juce::Rectangle<int> area; int track = -1; };
        std::vector<Line> lines;
    };
    AppContext& ctx;
    juce::String heading, sub;
    std::vector<DetailSection> sections;
    std::vector<TrackInfo> tracks;
    juce::Rectangle<int> panel;
    juce::Viewport viewport;
    Body body { *this };
    friend struct Body;
};

} // namespace nx
