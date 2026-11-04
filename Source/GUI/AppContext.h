// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// What every UI component can reach: the services and the navigation / playback actions.
#pragma once

#include "../Core/Perf.h"
#include "../Library/Library.h"
#include "../Engine/PlaybackEngine.h"
#include "../Ext/ThemeEngine.h"
#include "../DSP/HeadphoneEq.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace nx
{

class UnifiedMediaProvider;
class PluginManager;
class AudioAnalyzer;
class ArtworkCache;
class LibraryScanner;
class DownloadManager;
class ArtistInfoService;
class RemoteServer;
struct StudioToolsState;
class TrackAnalyzer;
struct TrackAnalysis;
class LicenseManager;
class UpdateChecker;

/** Why the "Premium" window is shown (NX Player Free). */
enum class UpsellReason { StudioMaster, ExclusiveMode, DeviceProfiles, Remote, MetadataEditor };

enum class PageId { Home, Search, RecentlyAdded, Albums, AlbumDetail, Artists, ArtistDetail, Songs, Playlists, PlaylistDetail,
                    Source, NowPlaying, Settings, Marketplace, UserPlugins,
                    Library,        // a = pill: albums | artistes | pistes | genres | recents | dxd | dsd | hires | cd
                    Genre,          // a = genre
                    Favourites };

struct PageRequest
{
    PageId page = PageId::Home;
    juce::String a, b;        // album artist / album, artist, playlist id, source id (+ NAS folder), search text, tab
    bool operator== (const PageRequest& o) const noexcept { return page == o.page && a == o.a && b == o.b; }
    bool operator!= (const PageRequest& o) const noexcept { return ! operator== (o); }
};

struct AppSettings
{
    juce::File file;
    juce::var root { new juce::DynamicObject() };

    void load()                { auto v = juce::JSON::parse (file); if (v.isObject()) root = v; }
    void save() const          { file.getParentDirectory().createDirectory(); file.replaceWithText (juce::JSON::toString (root)); }
    juce::var get (const juce::String& k, const juce::var& def = {}) const { return root.getProperty (k, def); }
    void set (const juce::String& k, const juce::var& v) { root.getDynamicObject()->setProperty (k, v); save(); }
};

class AppContext
{
public:
    virtual ~AppContext() = default;

    virtual Library& library() = 0;
    virtual PlaybackEngine& engine() = 0;
    virtual UnifiedMediaProvider& providers() = 0;
    virtual ThemeManager& themes() = 0;
    virtual PluginManager& plugins() = 0;
    virtual AudioAnalyzer& analyzer() = 0;
    virtual ArtworkCache& artwork() = 0;
    virtual LibraryScanner& scanner() = 0;
    virtual AppSettings& settings() = 0;
    virtual DownloadManager& downloads() = 0;
    virtual ArtistInfoService& artistInfo() = 0;
    virtual RemoteServer& remote() = 0;
    virtual StudioToolsState& studioTools() = 0;
    /** Background scan of the current track (waveform, Peak / RMS / DR); broadcasts a change when one finishes. */
    virtual TrackAnalyzer& trackAnalyzer() = 0;
    /** The current track's analysis (null while scanning, or for streams). */
    virtual std::shared_ptr<const TrackAnalysis> currentAnalysis() = 0;

    const Theme& theme() { return themes().current(); }

    /** Plays a list (local, NAS, Qobuz / Tidal through the engine; Spotify / YouTube handed over). */
    virtual void playTracks (const std::vector<TrackInfo>& tracks, int startIndex) = 0;
    virtual void enqueue (const std::vector<TrackInfo>& tracks) = 0;
    /** Plays a whole collection ("local" = the entire library, "favourites"); shuffle = endless random passes. */
    virtual void playCollection (const juce::String& scope, bool shuffle) = 0;
    virtual const TrackInfo* currentTrack() = 0;
    virtual void navigate (const PageRequest& request) = 0;
    virtual void toggleMiniPlayer() = 0;
    /** Streamer "Now Playing" card in its own small window (for OBS Window Capture). */
    virtual void setNowPlayingOverlay (bool show) = 0;
    virtual bool isNowPlayingOverlayVisible() = 0;
    virtual void setShuffle (bool on) = 0;
    virtual void setRepeat (PlaybackEngine::Repeat mode) = 0;
    virtual bool isShuffle() const = 0;
    virtual PlaybackEngine::Repeat getRepeat() const = 0;
    virtual void applyEngineSettings() = 0;
    virtual juce::StringArray getBackendNames() = 0;
    virtual juce::String getCurrentBackendName() = 0;
    virtual void selectBackend (const juce::String& name) = 0;
    virtual void selectDevice (const juce::String& name) = 0;
    /** Saves the chosen theme and offers a restart (no hot re-skin: it would invalidate LookAndFeel pointers). */
    virtual void applyTheme (const juce::String& id) = 0;
    /** Community themes: imports a theme file (.json, .nxtheme folder or .zip) into the user's themes and applies it
        (returns its id, or "" with `error`); exports the current theme, accent included, as one .json file. */
    virtual juce::String importThemeFile (const juce::File& file, juce::String& error) = 0;
    virtual bool exportCurrentTheme (const juce::File& destination) = 0;
    /** The file choosers of the two buttons (Extensions & Thèmes, Réglages › Interface). */
    void chooseThemeToImport();
    void chooseThemeExport();
    /** Appearance: "dark", "light" or "system" (follows the OS), and the accent colour
        (transparent = the theme's own). Both are applied at once and remembered. */
    virtual void setAppearance (const juce::String& mode) = 0;
    virtual juce::String getAppearance() = 0;
    virtual void setAccent (juce::Colour accent) = 0;
    virtual juce::Colour getAccent() = 0;
    /** The theme that will be active at the next launch (the running one until a restart). */
    virtual juce::String selectedThemeId() { return theme().id; }
    virtual void showMessage (const juce::String& text) = 0;
    /** User data folder (settings, library, user themes and plug-ins) and bundled resources folder. */
    virtual juce::File userDataDirectory() = 0;
    virtual juce::File resourcesDirectory() = 0;
    /** Remembers the NAS servers found so far (they reconnect at the next launch without discovery). */
    virtual void saveNasServers() = 0;

    /** Studio tools changed: rebuilds the DSP chain (DSD is converted to PCM while a tool is on). */
    virtual void applyStudioTools() = 0;
    /** Headphone EQ of the AURA DSP panel: preset index (-1 = off) and bypass, applied at once and remembered. */
    virtual void setHeadphoneEq (int presetIndex, bool bypass) = 0;
    /** Parametric EQ (7 bands): applied at once (live, while dragging); `persist` also saves it in settings.json. */
    virtual void setParametricEq (const hpeq::ParametricBands& bands, bool enabled, bool persist) = 0;
    /** User presets of the parametric EQ, kept in settings.json ("peqPresets"). */
    virtual juce::StringArray getParametricPresetNames() = 0;
    virtual void saveParametricPreset (const juce::String& name) = 0;
    virtual bool loadParametricPreset (const juce::String& name) = 0;
    /** AURA DSP: the "NX Precision Audio Control" panel (source, headroom, sample rate conversion, filters). */
    virtual void showAuraDsp() = 0;
    /** Official links (shop, Discord community); an empty Discord address hides its buttons. */
    virtual juce::String shopUrl() = 0;
    virtual juce::String discordUrl() = 0;
    void openShop() { juce::URL (shopUrl()).launchInDefaultBrowser(); }
    void openDiscord() { if (discordUrl().isNotEmpty()) juce::URL (discordUrl()).launchInDefaultBrowser(); }
    /** Volume of the player bar, 0..1 (1 = 100 % = bit-perfect). */
    virtual void setUserVolume (double v) = 0;
    virtual double getUserVolume() = 0;
    /** NX Remote on / off (QR code in the settings and the top bar). */
    virtual bool setRemoteEnabled (bool on) = 0;
    /** Interface language ("fr" / "en"): rebuilds the visible texts. */
    virtual void setLanguage (const juce::String& code) = 0;
    /** Layout presets of the onboarding / settings: sidebar "wide" | "compact", player "bottom" | "top". */
    virtual void setLayoutOption (const juce::String& key, const juce::var& value) = 0;
    virtual bool isSleepBlocked() = 0;
    /** Streamer / Gaming mode: interface refresh reduced to a minimum (saved in the settings). */
    virtual bool isStreamerMode() = 0;
    virtual void setStreamerMode (bool on) = 0;
    /** Easter egg (three clicks on the logo): adds the hidden ZEYN theme to the list (not applied), remembered. */
    virtual void unlockZeynTheme() = 0;

    //== Free / Premium and updates ==========================================
    virtual LicenseManager& license() = 0;
    virtual bool isPremium() = 0;
    /** Checks and saves a licence key (NXP1-...); false with the reason. */
    virtual bool activateLicense (const juce::String& key, juce::String& error) = 0;
    virtual void deactivateLicense() = 0;
    /** The "Passez à NX Player Premium" window (enter a key / buy). */
    virtual void showUpsell (UpsellReason why) = 0;
    virtual UpdateChecker& updates() = 0;
    /** userAsked: also says "à jour" / the error; otherwise only an available update is announced. */
    virtual void checkForUpdates (bool userAsked) = 0;
    virtual bool isZeynUnlocked() = 0;
    /** A reassignable shortcut typed in another window (mini player): true if it was one. */
    virtual bool handleShortcutKey (const juce::KeyPress&) { return false; }
    /** First-run assistant (also from Réglages › Interface). */
    virtual void showOnboarding() = 0;
    virtual void setImmersive (bool on) = 0;
    /** Analyse / convert in AuraEngine X (local files). */
    virtual void sendToAuraEngine (const std::vector<TrackInfo>& tracks) = 0;
    /** "NAS", "Jellyfin" or the server name for a track / album source ("upnp:<udn>"); empty for local files. */
    virtual juce::String networkSourceLabel (const juce::String& source) = 0;

    /** The blurred artwork behind the window (for glass panels), in main-window coordinates. */
    virtual const juce::Image& backdrop() = 0;
    virtual juce::Component* backdropOwner() = 0;
    /** Layout overrides requested by plug-ins / settings. */
    juce::var layout { new juce::DynamicObject() };
};

} // namespace nx
