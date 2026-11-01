// NX Player — MIT License
// Owns every service and the windows; implements AppContext for the interface.
#pragma once

#include <optional>
#include "../Core/LicenseManager.h"
#include "../Core/UpdateChecker.h"

#include "AppContext.h"
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
class MiniWindow;
class MainComponent;

class AppController final : public AppContext, private juce::Timer
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
    TrackAnalyzer& trackAnalyzer() override    { return *trackScan; }
    std::shared_ptr<const TrackAnalysis> currentAnalysis() override { return analysisPath.isNotEmpty() ? trackScan->get (analysisPath) : nullptr; }

    void playTracks (const std::vector<TrackInfo>& tracks, int startIndex) override;
    void enqueue (const std::vector<TrackInfo>& tracks) override;
    void playCollection (const juce::String& scope, bool shuffle) override;
    const TrackInfo* currentTrack() override;
    void navigate (const PageRequest& r) override;
    void toggleMiniPlayer() override;
    void setShuffle (bool on) override;
    void setRepeat (PlaybackEngine::Repeat mode) override;
    bool isShuffle() const override         { return eng->isShuffle(); }
    PlaybackEngine::Repeat getRepeat() const override { return eng->getRepeat(); }
    void applyEngineSettings() override;
    juce::StringArray getBackendNames() override;
    juce::String getCurrentBackendName() override;
    void selectBackend (const juce::String& name) override;
    void selectDevice (const juce::String& name) override;
    void applyTheme (const juce::String& id) override;
    juce::String selectedThemeId() override { return appSettings.get ("theme", themeManager.current().id).toString(); }
    /** Immediate re-skin (tests only: the UI never calls it). */
    void applyThemeNow (const juce::String& id);
    /** Relaunches the application once this process has quit. */
    void restartApplication();
    void showMessage (const juce::String& text) override;
    juce::File userDataDirectory() override  { return dataDir; }
    juce::File resourcesDirectory() override { return resourcesDir; }
    void saveNasServers() override;
    const juce::Image& backdrop() override;
    juce::Component* backdropOwner() override;
    void applyStudioTools() override;
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
    void showOnboarding() override;
    void setImmersive (bool on) override;
    void sendToAuraEngine (const std::vector<TrackInfo>& tracks) override;
    juce::String networkSourceLabel (const juce::String& source) override;

    /** Remote commands (also used by the tests). Message thread. */
    void handleRemoteCommand (const juce::String& command, const juce::StringPairArray& params);
    juce::String buildRemoteState();
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
    std::shared_ptr<StudioToolsState> studio = std::make_shared<StudioToolsState>();
    std::unique_ptr<TrackAnalyzer> trackScan = std::make_unique<TrackAnalyzer>();   // outlives the windows (listeners)
    juce::String analysisPath;                     // local file of the current track
    SleepBlocker sleepBlocker;
    double lastRemoteState = 0.0;
    double lastNowPlayingUpdate = 0.0;
    juce::String nowPlayingArtworkSent;
    ThemeManager themeManager;
    PluginManager pluginManager;
    NXLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<MiniWindow> miniWindow;
    std::unique_ptr<MainComponent> headlessMain;   // tests without a window
    std::unique_ptr<LicenseManager> licenseManager;
    std::unique_ptr<UpdateChecker> updateChecker;
    int upsellCount = 0;
    UpsellReason lastUpsell = UpsellReason::StudioMaster;
    bool upsellOpen = false;
    void applyEdition();
    /** NX Player Free: the Studio Master tracks are left out of a list (the start index follows). */
    std::vector<TrackInfo> withoutStudioMasters (const std::vector<TrackInfo>& tracks, int& startIndex);

    std::vector<TrackInfo> queueTracks;            // parallel to the engine queue
    TrackInfo externalTrack;                        // Spotify / YouTube hand-off
    bool externalPlaying = false;
    bool restartPromptOpen = false;
    // Endless shuffle: shuffled passes over the whole collection, queued a chunk at a time.
    juce::String shuffleScope;                      // empty = no endless shuffle running
    std::vector<TrackInfo> shufflePass;             // current pass, already shuffled
    size_t shuffleNext = 0;                         // next track of the pass to queue
    juce::int64 lastQueuedKey = 0;
    std::mt19937 rng { std::random_device{}() };
    juce::String backendName;
    double lastFrame = 0.0;
    int lyricsGeneration = 0;
    juce::Image emptyImage;
    std::shared_ptr<int> alive = std::make_shared<int> (0);
    std::atomic<bool> nasDiscoveryRunning { false };
};

} // namespace nx
