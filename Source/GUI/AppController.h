// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Owns every service and the windows; implements AppContext for the interface.
#pragma once

#include <set>

#include <optional>
#include "../Core/LicenseManager.h"
#include "../Core/UpdateChecker.h"
#include "../Core/Links.h"

#include "AppContext.h"
#include "../DSP/E9ds.h"
#include "Waveform.h"
#include "../Core/Perf.h"
#include <random>
#include <algorithm>
#include "LookAndFeel.h"
#include "Artwork.h"
#include "../Library/Library.h"
#include "../Library/LibraryScanner.h"
#include "../Library/Lyrics.h"
#include "../Library/Downloads.h"
#include "../Platform/MediaControls.h"
#include "../Providers/MediaProvider.h"
#include "../Ext/ThemeEngine.h"
#include "../Ext/PluginManager.h"
#include "../Ext/AudioAnalyzer.h"
#include "../Library/ArtistInfo.h"
#include "../Remote/RemoteServer.h"
#include "../DSP/StudioTools.h"
#include "../Platform/SystemMonitor.h"

namespace nx
{

class MainWindow;
class OverlayWindow;
class MiniWindow;
class MainComponent;

class AppController final : public AppContext, private juce::Timer, private juce::DarkModeSettingListener, private juce::ChangeListener
{
public:
    struct Options
    {
        juce::File dataDir;                          // empty = user data folder
        juce::File resourcesDir;                     // empty = next to the executable / bundle Resources
        std::unique_ptr<OutputBackend> backend;      // tests: virtual DAC
        bool createWindow = true;
        std::optional<licensing::PublicKey> licensePublicKey;   // tests: their own key pair (default: the embedded key)
    };

    AppController();
    explicit AppController (Options options);
    ~AppController() override;

    void openFiles (const juce::StringArray& paths);

    //== AppContext ===========================================================
    Library& library() override                { return lib; }
    PlaybackEngine& engine() override          { return *eng; }
    UnifiedMediaProvider& providers() override { return *unified; }
    ThemeManager& themes() override            { return themeManager; }
    PluginManager& plugins() override          { return pluginManager; }
    AudioAnalyzer& analyzer() override         { return *audioAnalyzer; }
    ArtworkCache& artwork() override           { return *artworkCache; }
    LibraryScanner& scanner() override         { return *libScanner; }
    AppSettings& settings() override           { return appSettings; }
    DownloadManager& downloads() override      { return *downloadManager; }
    ArtistInfoService& artistInfo() override   { return *artistInfoService; }
    RemoteServer& remote() override            { return *remoteServer; }
    StudioToolsState& studioTools() override   { return *studio; }
    E9dsState& e9ds() override                 { return *e9dsState; }
    TrackAnalyzer& trackAnalyzer() override    { return *trackScan; }
    std::shared_ptr<const TrackAnalysis> currentAnalysis() override { return analysisPath.isNotEmpty() ? trackScan->get (analysisPath) : nullptr; }

    void playTracks (const std::vector<TrackInfo>& tracks, int startIndex) override;
    void enqueue (const std::vector<TrackInfo>& tracks) override;
    void playCollection (const juce::String& scope, bool shuffle) override;
    const TrackInfo* currentTrack() override;
    void navigate (const PageRequest& r) override;
    void toggleMiniPlayer() override;
    void setNowPlayingOverlay (bool show) override;
    bool isNowPlayingOverlayVisible() override;
    /** Streamer mode: now_playing.txt ("Artiste — Titre") and now_playing_cover.jpg in `dir` (empty text when stopped). */
    static bool writeNowPlayingFiles (const juce::File& dir, const TrackInfo* track, const juce::Image& cover);
    /** Message thread, from the timer: queues the OBS files when the track / artwork changed (returns at once). */
    void exportNowPlaying();
    bool isExportingNowPlaying() const { return obsWriter.getNumJobs() > 0; }
    int getTickIntervalMs() const { return getTimerInterval(); }
    void tickForTests() { timerCallback(); }
    OverlayWindow* getOverlayWindow() { return overlayWindow.get(); }
    void setShuffle (bool on) override;
    bool toggleCurrentFavourite() override;
    void setRepeat (PlaybackEngine::Repeat mode) override;
    bool isShuffle() const override         { return eng->isShuffle(); }
    PlaybackEngine::Repeat getRepeat() const override { return eng->getRepeat(); }
    void applyEngineSettings() override;
    juce::StringArray getBackendNames() override;
    juce::String getCurrentBackendName() override;
    void selectBackend (const juce::String& name) override;
    void selectDevice (const juce::String& name) override;
    void applyTheme (juce::String id) override;
    juce::String selectedThemeId() override { return appSettings.get ("theme", themeManager.current().id).toString(); }
    /** Immediate re-skin (tests only: the UI never calls it). */
    void applyThemeNow (juce::String id);
    juce::String importThemeFile (const juce::File& file, juce::String& error) override;
    bool exportCurrentTheme (const juce::File& destination) override;
    void setAppearance (const juce::String& mode) override;
    juce::String getAppearance() override { return appSettings.get ("appearance", "dark").toString(); }
    void setAccent (juce::Colour accent) override;
    juce::Colour getAccent() override { return themeManager.getAccentOverride(); }
    /** The theme the appearance stands for (dark: the chosen dark theme, light: Pure Light, system: the OS). */
    juce::String themeForAppearance();
    static constexpr const char* lightThemeId = "pure-light";
    /** Relaunches the application once this process has quit. */
    void restartApplication();
    void showMessage (const juce::String& text) override;
    juce::File userDataDirectory() override  { return dataDir; }
    juce::File resourcesDirectory() override { return resourcesDir; }
    void saveNasServers() override;
    const juce::Image& backdrop() override;
    juce::Component* backdropOwner() override;
    void applyStudioTools() override;
    void setHeadphoneEq (int presetIndex, bool bypass) override;
    void setParametricEq (const hpeq::ParametricBands& bands, bool enabled, bool persist) override;
    void setE9ds (bool enabled, double focusHz, double amountDb, int mode) override;
    void setAutoTruePeak (bool on, double ceilingDb) override;
    void setSmartComfort (bool on) override;
    void updateDspNeedsPcm();
    juce::StringArray getParametricPresetNames() override;
    void saveParametricPreset (const juce::String& name) override;
    bool loadParametricPreset (const juce::String& name) override;
    static juce::var bandsToVar (const hpeq::ParametricBands& b);
    static hpeq::ParametricBands bandsFromVar (const juce::var& v);
    void showAuraDsp() override;
    void showArtwork (const juce::String& artworkKey, const juce::String& title, const juce::String& subtitle) override;
    void showDetails (const std::vector<TrackInfo>& tracks, const juce::String& albumTitle) override;
    juce::String shopUrl() override    { return links::resolve (appSettings.get ("purchaseUrl"), links::shop); }
    juce::String discordUrl() override { return links::resolve (appSettings.get ("discordUrl"), links::discord); }
    void setUserVolume (double v) override;
    double getUserVolume() override;
    bool setRemoteEnabled (bool on) override;
    void setLanguage (const juce::String& code) override;
    void setLayoutOption (const juce::String& key, const juce::var& value) override;
    bool isSleepBlocked() override { return sleepBlocker.isActive(); }
    bool isStreamerMode() override { return perf::streamer(); }
    void setStreamerMode (bool on) override;
    void unlockZeynTheme() override;
    bool isZeynUnlocked() override;
    bool handleShortcutKey (const juce::KeyPress& k) override;
    LicenseManager& license() override { return *licenseManager; }
    bool isPremium() override;
    bool activateLicense (const juce::String& key, juce::String& error) override;
    void deactivateLicense() override;
    void showUpsell (UpsellReason why) override;
    UpdateChecker& updates() override { return *updateChecker; }
    void checkForUpdates (bool userAsked) override;
    int getUpsellCount() const noexcept { return upsellCount; }   // tests
    UpsellReason getLastUpsell() const noexcept { return lastUpsell; }
    static juce::String upsellTitle (UpsellReason why);
    static juce::String upsellMessage (UpsellReason why);
    /** Text of the "update available" window: installed version, new version and its release notes. */
    static juce::String updateMessage (const UpdateInfo& info, const juce::String& currentVersion, const juce::String& language);
    void showOnboarding() override;
    void setImmersive (bool on) override;
    void sendToAuraEngine (const std::vector<TrackInfo>& tracks) override;
    juce::String networkSourceLabel (const juce::String& source) override;

    /** Remote commands (also used by the tests). Message thread. */
    void handleRemoteCommand (const juce::String& command, const juce::StringPairArray& params);
    juce::String buildRemoteState();
    /** The active theme for Aura Remote: id, name and exact colours (hex), isLight. */
    static juce::var remoteTheme (const Theme& theme);
    juce::String buildRemoteLibrary (const juce::StringPairArray& params);
    /** Cover for the phone: baseline RGB JPEG, at most maxSize x maxSize; key = artwork key or "track:<id>". Empty = none. */
    juce::MemoryBlock remoteArtwork (const juce::String& key, int maxSize);

    MainComponent* getMainComponent();
    MainWindow* getMainWindow() { return mainWindow.get(); }
    juce::File getDataDir() const { return dataDir; }
    static juce::File defaultDataDir();
    static juce::File defaultResourcesDir();

private:
    void timerCallback() override;
    void darkModeSettingChanged() override { if (getAppearance() == "system") applyThemeNow (themeForAppearance()); }
    void onTrackChanged();
    void loadLyrics (const TrackInfo& t);
    void saveEngineSettings();
    void updateSystemNowPlaying (bool force);
    void loadEngineSettings();
    void rebuildDspChain();
    void applyLayoutSettings();
    std::vector<TrackInfo> collectionTracks (const juce::String& scope);
    void extendShuffle();

    juce::File dataDir, resourcesDir;
    AppSettings appSettings;
    Library lib;
    std::unique_ptr<LibraryScanner> libScanner;
    std::unique_ptr<CredentialStore> credentials;
    std::unique_ptr<UnifiedMediaProvider> unified;
    std::unique_ptr<PlaybackEngine> eng;
    std::unique_ptr<AudioAnalyzer> audioAnalyzer;
    std::unique_ptr<ArtworkCache> artworkCache;
    std::unique_ptr<DownloadManager> downloadManager;
    std::unique_ptr<MediaControls> mediaControls;
    std::unique_ptr<ArtistInfoService> artistInfoService;
    std::unique_ptr<RemoteServer> remoteServer;
    std::mutex remoteArtLock;
    std::vector<std::pair<juce::String, juce::MemoryBlock>> remoteArtCache;
    std::set<juce::String> remoteHttpArt;       // online covers handed to the phone: the only URLs /api/art will fetch
    juce::String remoteArtKey (const juce::String& key);
    std::shared_ptr<StudioToolsState> studio = std::make_shared<StudioToolsState>();
    std::shared_ptr<E9dsState> e9dsState = std::make_shared<E9dsState>();
    std::unique_ptr<TrackAnalyzer> trackScan = std::make_unique<TrackAnalyzer>();   // outlives the windows (listeners)
    juce::String analysisPath;                     // local file of the current track
    /** A scan finished: its True Peak goes to the library and the engine; then the next track of the queue is measured. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::String localPathOf (const TrackInfo& t);       // local file or offline copy (empty for streams)
    PlayableItem playable (const TrackInfo& t);          // toPlayable + the stored True Peak
    SleepBlocker sleepBlocker;
    double lastRemoteState = 0.0;
    double lastNowPlayingUpdate = 0.0;
    juce::String nowPlayingArtworkSent;
    ThemeManager themeManager;
    PluginManager pluginManager;
    NXLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<MiniWindow> miniWindow;
    std::unique_ptr<OverlayWindow> overlayWindow;
    juce::String exportedNowPlaying;        // what the OBS files hold ("" = not written yet)
    juce::ThreadPool obsWriter { 1 };        // writes now_playing.txt / .jpg
    PlaybackActivity appActivity;           // macOS: no App Nap / timer coalescing while music plays
    std::unique_ptr<MainComponent> headlessMain;   // tests without a window
    std::unique_ptr<LicenseManager> licenseManager;
    std::unique_ptr<UpdateChecker> updateChecker;
    int upsellCount = 0;
    UpsellReason lastUpsell = UpsellReason::StudioMaster;
    bool upsellOpen = false, updateOpen = false;
    void showUpdateAvailable (const UpdateInfo& info, bool userAsked);
    void applyEdition();
    /** Aura Free: the Studio Master tracks are left out of a list (the start index follows). */
    std::vector<TrackInfo> withoutStudioMasters (const std::vector<TrackInfo>& tracks, int& startIndex);

    std::vector<TrackInfo> queueTracks;            // parallel to the engine queue
    TrackInfo externalTrack;                        // Spotify / YouTube hand-off
    bool externalPlaying = false;
    // Endless shuffle: shuffled passes over the whole collection, queued a chunk at a time.
    juce::String shuffleScope;                      // empty = no endless shuffle running
    std::vector<TrackInfo> shufflePass;             // current pass, already shuffled
    size_t shuffleNext = 0;                         // next track of the pass to queue
    juce::int64 lastQueuedKey = 0;
    std::mt19937 rng { std::random_device{}() };
    juce::String backendName;
    double lastFrame = 0.0;
    juce::Image emptyImage;
    std::shared_ptr<int> alive = std::make_shared<int> (0);
    std::atomic<bool> nasDiscoveryRunning { false };
};

} // namespace nx
