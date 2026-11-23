// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#pragma once

#include "Style.h"
#include "Artwork.h"
#include "Waveform.h"
#include <map>
#include <set>

namespace nx
{

class IconButton : public juce::Button
{
public:
    IconButton (style::Icon i, const juce::String& tooltip, AppContext& c) : juce::Button (tooltip), icon (i), ctx (c)
    {
        setTooltip (tooltip);
        setWantsKeyboardFocus (false);   // the space bar stays play / pause instead of re-clicking the button
    }
    void setIcon (style::Icon i) { icon = i; repaint(); }
    bool active = false;          // accent colour (shuffle on, lyrics shown...)
    bool one = false;             // small "1" on the icon (repeat one)
    bool pro = false;             // "PRO" pill: a Premium feature in Aura Free
    bool primary = false;         // big round play button
    float glyph = 0.62f;          // icon size relative to the button (the player bar enlarges it on wide screens)
    void paintButton (juce::Graphics& g, bool hi, bool down) override;
private:
    style::Icon icon;
    AppContext& ctx;
};

/** Explanation of an advanced feature (the "+" buttons of AURA DSP and Settings › Audio), translated. */
struct Explainer { juce::String title, what, gain, limits; };
/** Ids: e9ds, dxd, crossfeed, headroom, hpeq, rate, fades, pcmdsd, keepdsd. Empty title for an unknown id. */
Explainer explainer (const juce::String& id);
juce::StringArray explainerIds();

/** Small round "+" next to a feature: opens a popover with what it does, what it brings, its limits. */
class InfoButton : public juce::Button
{
public:
    InfoButton (AppContext& c, juce::String id);
    void paintButton (juce::Graphics& g, bool hi, bool down) override;
    void clicked() override;
    const juce::String id;
    static constexpr int size = 18;
    /** Tests: the popover last opened (null once closed). */
    static juce::Component* lastPopover();
private:
    AppContext& ctx;
};

/** Heart of the track being played (Now Playing, immersive view): filled accent when it is a favourite. */
void syncHeart (IconButton& heart, const TrackInfo* track);

class ArtworkView : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener
{
public:
    ArtworkView (AppContext& c, int pixelSize);
    ~ArtworkView() override;
    void setArtwork (const juce::String& key, const juce::String& placeholderText);
    void paint (juce::Graphics&) override;
    float cornerRadius = 10.0f;
    bool shadow = true;
    /** Set: the artwork is clickable (hand cursor), e.g. to show it full size. */
    void setOnClick (std::function<void()> f) { onClick = std::move (f); setMouseCursor (onClick ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); }
    void mouseUp (const juce::MouseEvent& e) override { if (onClick && e.mouseWasClicked() && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition())) onClick(); }
    const juce::String& getKey() const noexcept { return key; }
    void click() { if (onClick) onClick(); }                    // tests
private:
    std::function<void()> onClick;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    AppContext& ctx;
    int size;
    juce::String key, placeholderText;
    juce::Image placeholderImage;
};

/** Format + quality pill of the player bar; click shows the Signal Path. */
class QualityBadge : public juce::Component
{
public:
    explicit QualityBadge (AppContext& c) : ctx (c) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void update (const juce::String& text, PathQuality q) { label = text; quality = q; repaint(); }
    float uiScale = 1.0f;                      // wide screens (PlayerBar::setUiScale)
    const juce::String& getLabel() const noexcept { return label; }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }
    std::function<void()> onClick;
private:
    AppContext& ctx;
    juce::String label;
    PathQuality quality = PathQuality::BitPerfect;
};

class SeekBar : public juce::Component
{
public:
    explicit SeekBar (AppContext& c) : ctx (c) {}
    void setPosition (double pos, double duration) { if (! dragging) { position = pos; length = duration; repaint(); } }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    std::function<void (double)> onSeek;
    bool showTimes = true;
    /** Waveform mode: the track's analysis replaces the progress line (plain line while it is computed). */
    void setWaveform (bool on, std::shared_ptr<const TrackAnalysis> a) { waveform = on; analysis = std::move (a); repaint(); }

    /** A-B loop markers (seconds, −1 = not set). Dragging a marker calls onLoopEdited (a, b). */
    void setLoop (double a, double b) { if (draggingMarker == 0) { loopA = a; loopB = b; repaint(); } }
    std::function<void (double, double)> onLoopEdited;
    void mouseMove (const juce::MouseEvent& e) override;
private:
    double xToTime (float x) const;
    float timeToX (double t) const;
    int markerAt (float x) const;      // 1 = A, 2 = B, 0 = none
    AppContext& ctx;
    double position = 0, length = 0;
    double loopA = -1.0, loopB = -1.0;
    bool dragging = false;
    bool waveform = false;
    std::shared_ptr<const TrackAnalysis> analysis;
    WaveformPainter painter;
    int draggingMarker = 0;
};

/** The [A-B] button of the player bar: first click = A, second = B (loop on), third = off. */
class LoopButton : public juce::Button
{
public:
    explicit LoopButton (AppContext& c) : juce::Button ("A-B"), ctx (c)
    {
        setWantsKeyboardFocus (false);
        setTooltip ("Boucle A-B : 1er clic = début (I), 2e = fin et boucle (O), 3e = arrêt (L)"_t);
    }
    int stage = 0;   // 0 off, 1 A set, 2 looping
    void paintButton (juce::Graphics& g, bool hi, bool down) override;
private:
    AppContext& ctx;
};

/** Hosts the UI plug-ins registered for one slot of the interface. */
class PluginSlot : public juce::Component, private juce::Timer
{
public:
    PluginSlot (AppContext& c, juce::String slotName, bool horizontal = false);
    void paint (juce::Graphics&) override;
    void refreshPlugins();
    int getPreferredHeight() const;
    bool isEmpty() const { return ids.isEmpty(); }
private:
    void timerCallback() override { perf::retune (*this, juce::jmax (1, fpsNormal), 2); if (isShowing()) repaint(); }
    AppContext& ctx;
    juce::String slot;
    bool horizontal;
    int fpsNormal = 0;
    juce::StringArray ids;
};

struct AlbumCardData
{
    AlbumInfo album;
    juce::String subtitle;
};

class AlbumGrid : public juce::Component, private juce::ChangeListener
{
public:
    explicit AlbumGrid (AppContext& c);
    ~AlbumGrid() override;
    void setAlbums (std::vector<AlbumInfo> albums);
    /** Height needed at a given width. */
    int heightForWidth (int width) const;
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::function<void (const AlbumInfo&)> onOpen, onPlay;
    /** Right click: the selection (at least the clicked card). Default: the album menu (playlists, NAS clean-up...). */
    std::function<void (const std::vector<AlbumInfo>&)> onContextMenu;
    /** Pencil on hover (library albums): opens the metadata editor. */
    std::function<void (const AlbumInfo&)> onEdit;
    /** Card width in pixels (0 = the theme's --nx-card-size) and the vertical list view. */
    void setCardSize (float px) { cardSize = px; repaint(); }
    void setListMode (bool on) { listMode = on; hover = -1; repaint(); }
    bool isListMode() const noexcept { return listMode; }
    float getCardSize() const noexcept { return cardSize; }
    /** Sort mode applied by setAlbums ("title" | "artist" | "added" | "format", empty = as given) for library albums. */
    juce::String sortMode;
    int maxRows = 0;          // 0 = unlimited
    bool roundArt = false;    // artists: round medallions
    bool libraryAlbums = true;// cards are library albums (context menu, drag to a playlist, NAS badge)
    const std::vector<AlbumInfo>& getAlbums() const noexcept { return albums; }
    std::vector<AlbumInfo> getSelection() const;
    void selectAll();
    void clearSelection() { selected.clear(); repaint(); }
    int getNumSelected() const noexcept { return (int) selected.size(); }
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    juce::Rectangle<int> cardBounds (int index) const;
    int columns() const;
    int indexAt (juce::Point<int>) const;
    AppContext& ctx;
    std::vector<AlbumInfo> albums;
    std::set<int> selected;
    juce::Rectangle<int> editRect (int index) const;
    void paintRow (juce::Graphics&, int index, juce::Rectangle<int> row);
    int hover = -1, anchor = -1;
    bool dragStarted = false, listMode = false;
    float cardSize = 0.0f;
    static constexpr int rowHeight = 60;
};

/** Horizontal pills (library filters: Albums | Artistes | Pistes | Genres | Ajouts récents | DXD | DSD | Hi-Res | CD). */
class PillBar : public juce::Component
{
public:
    explicit PillBar (AppContext& c) : ctx (c) { setRepaintsOnMouseActivity (true); }
    void setPills (juce::StringArray ids, juce::StringArray labels, const juce::String& active);
    void setCounts (std::map<juce::String, int> c) { counts = std::move (c); repaint(); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::function<void (const juce::String&)> onSelect;
    juce::String getActive() const { return active; }
    int getNumPills() const { return ids.size(); }
    juce::Rectangle<int> pillBounds (int i) const { return i >= 0 && i < (int) bounds.size() ? bounds[(size_t) i] : juce::Rectangle<int>(); }
private:
    void layoutPills();
    AppContext& ctx;
    juce::StringArray ids, labels;
    juce::String active;
    std::map<juce::String, int> counts;
    std::vector<juce::Rectangle<int>> bounds;
};

class TrackList : public juce::Component, private juce::ChangeListener
{
public:
    explicit TrackList (AppContext& c);
    ~TrackList() override;
    void setTracks (std::vector<TrackInfo> tracks, bool showArtwork = true, bool showAlbum = true, bool numbered = false);
    int getTotalHeight() const { return (int) tracks.size() * rowHeight + 8; }
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    const std::vector<TrackInfo>& getTracks() const noexcept { return tracks; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    static constexpr int rowHeight = 54;
    /** Playlist page: the id of the playlist shown ("Retirer de cette playlist"). */
    juce::int64 playlistContext = 0;
    std::function<void()> onChanged;      // favourites / playlist edited from the list
    std::function<void (const TrackInfo&)> onEdit;   // album page: pencil on each row (metadata editor)
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void download (const std::vector<TrackInfo>& list);
    AppContext& ctx;
    std::vector<TrackInfo> tracks;
    std::vector<juce::Rectangle<int>> downloadButtons;   // per row, filled while painting (remote tracks only)
    std::vector<juce::Rectangle<int>> heartButtons, editButtons;
    std::vector<juce::Rectangle<int>> artistRects;      // per row: clickable artist name (opens the artist page)
    bool artwork = true, album = true, numbered = false;
    int hover = -1, selected = -1;
    bool dragStarted = false;
};

/** Scrollable page content with a title. Children are laid out by the page. */
class ScrollPage : public juce::Component
{
public:
    explicit ScrollPage (AppContext& c);
    void resized() override;
    juce::Component& content() { return contentComp; }
    void setContentHeight (int h);
    /** Always open a page at its top (after the content has been laid out). */
    void scrollToTop() { viewport.setViewPosition (0, 0); }
    int getScrollY() const { return viewport.getViewPositionY(); }
    void setScrollY (int y) { viewport.setViewPosition (0, y); }
    virtual void layoutContent (int width) = 0;
    virtual void refresh() {}
    void paint (juce::Graphics&) override {}
protected:
    AppContext& ctx;
    juce::Viewport viewport;
    juce::Component contentComp;
};

void drawPageTitle (juce::Graphics& g, const Theme& t, juce::Rectangle<int> r, const juce::String& title, const juce::String& subtitle = {});

} // namespace nx
