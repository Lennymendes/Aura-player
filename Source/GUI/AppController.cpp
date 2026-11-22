// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "AppController.h"
#include "../Core/Diagnostics.h"
#include "../Core/ImageDecode.h"
#include "../Audio/SacdIso.h"
#include <numeric>
#include "Windows.h"
#include "../Providers/UpnpProvider.h"
#include "../Providers/StreamingProviders.h"
#include "../Output/CaptureBackend.h"
#include "../Engine/DeviceProfiles.h"
#include "../Platform/AuraBridge.h"

namespace nx
{

juce::File AppController::defaultDataDir()
{
   #if JUCE_MAC
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    const auto dir = base.getChildFile ("Aura");
    // renamed from NX Player: settings, library, licence and themes move with the user
    if (const auto old = base.getChildFile ("NX Player"); ! dir.exists() && old.isDirectory()) old.moveFileTo (dir);
    return dir;
}

juce::File AppController::defaultResourcesDir()
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
   #if JUCE_MAC
    auto res = exe.getParentDirectory().getParentDirectory().getChildFile ("Resources");
    if (res.getChildFile ("Themes").isDirectory()) return res;
   #endif
    return exe.getParentDirectory();
}

AppController::AppController() : AppController (Options()) {}

AppController::AppController (Options options)
{
    dataDir = options.dataDir != juce::File() ? options.dataDir : defaultDataDir();
    resourcesDir = options.resourcesDir != juce::File() ? options.resourcesDir : defaultResourcesDir();
    dataDir.createDirectory();
    appSettings.file = dataDir.getChildFile ("settings.json");
    appSettings.load();
    i18n::setLanguage (appSettings.get ("language", "fr").toString());
    applyLayoutSettings();
    perf::setStreamer ((bool) appSettings.get ("streamerMode", false));

    juce::String err;
    if (! lib.open (dataDir, err)) juce::Logger::writeToLog ("bibliothèque : "_t + err);
    libScanner = std::make_unique<LibraryScanner> (lib);

    credentials = std::make_unique<CredentialStore> (dataDir.getChildFile ("accounts.json"));
    unified = std::make_unique<UnifiedMediaProvider> (lib, *credentials);
    unified->addProvider (std::make_unique<UpnpProvider>());
   #if NX_ENABLE_STREAMING
    // Streaming services: off for now (cmake -DNX_ENABLE_STREAMING=ON brings them back).
    unified->addProvider (std::make_unique<QobuzProvider> (*credentials));
    unified->addProvider (std::make_unique<TidalProvider> (*credentials));
    unified->addProvider (std::make_unique<SpotifyProvider> (*credentials));
    unified->addProvider (std::make_unique<YouTubeMusicProvider> (*credentials));
   #endif

    // Output: the saved backend, else the best one of the platform, else a silent virtual DAC.
    std::unique_ptr<OutputBackend> backend = std::move (options.backend);
    if (backend == nullptr)
    {
        auto list = createPlatformBackends();
        const auto saved = appSettings.get ("backend").toString();
        for (auto& b : list) if (backend == nullptr && b->getTypeName() == saved) backend = std::move (b);
        for (auto& b : list) if (backend == nullptr && b != nullptr) backend = std::move (b);
        if (backend == nullptr)
        {
            auto silent = std::make_unique<CaptureBackend>();
            silent->record = false;
            silent->name = "Sortie silencieuse (aucun pilote audio)";
            backend = std::move (silent);
        }
    }
    backendName = backend->getTypeName();
    eng = std::make_unique<PlaybackEngine> (std::move (backend));
    eng->streamResolver = [this] (const PlayableItem& item) { return unified->openStream (item); };
    loadEngineSettings();

    // Free / Premium: one binary; the key saved in the settings (it survives application updates)
    licenseManager = options.licensePublicKey ? std::make_unique<LicenseManager> (*options.licensePublicKey) : std::make_unique<LicenseManager>();
    licenseManager->restore (appSettings.get ("licenseKey").toString());
    applyEdition();
    updateChecker = std::make_unique<UpdateChecker> (NX_VERSION);
    updateChecker->setUrl (appSettings.get ("updateUrl", UpdateChecker::defaultUrl).toString());

    audioAnalyzer = std::make_unique<AudioAnalyzer> (eng->getAnalyzer());
    artworkCache = std::make_unique<ArtworkCache> (lib, dataDir.getChildFile ("ArtworkWeb"));

    // Offline copies of NAS / Jellyfin tracks
    downloadManager = std::make_unique<DownloadManager> (lib);
    if (const auto folder = appSettings.get ("downloadFolder").toString(); folder.isNotEmpty())
        downloadManager->setFolder (juce::File (folder));
    downloadManager->onMessage = [this] (const juce::String& m) { showMessage (m); };

    // Artist pages (photos, biographies) and Aura Remote
    artistInfoService = std::make_unique<ArtistInfoService> (lib);
    artistInfoService->setEnabled ((bool) appSettings.get ("artistInfoOnline", true));
    artistInfoService->setAudioDbKey (appSettings.get ("audioDbKey").toString());   // no personal key: TheAudioDB is never called
    artistInfoService->language = i18n::getLanguage();
    remoteServer = std::make_unique<RemoteServer>();
    remoteServer->onCommand = [this] (const juce::String& c, const juce::StringPairArray& p) { handleRemoteCommand (c, p); };
    remoteServer->onLibraryQuery = [this] (const juce::StringPairArray& p) { return buildRemoteLibrary (p); };
    // Aura Remote identity (token + port): generated once and kept, so the QR code and the link never change by themselves.
    {
        remoteServer->setToken (appSettings.get ("remoteToken", "").toString());
        if (appSettings.get ("remoteToken", "").toString() != remoteServer->getToken()) appSettings.set ("remoteToken", remoteServer->getToken());
        remoteServer->onTokenChanged = [this] { appSettings.set ("remoteToken", remoteServer->getToken()); };
    }
    remoteServer->getArtwork = [this] (const juce::String& key, int maxSize, juce::String& mime) -> juce::MemoryBlock
    {
        mime = "image/jpeg";
        return remoteArtwork (key, maxSize);
    };

    // Themes
    themeManager.setZeynUnlocked ((bool) appSettings.get ("zeynUnlocked", false));
    themeManager.setSearchFolders ({ resourcesDir.getChildFile ("Themes"), dataDir.getChildFile ("Themes") });
    themeManager.onThemeChanged = [this] (const Theme& t)
    {
        lookAndFeel.applyTheme (t);
        // lookAndFeelChanged() everywhere: combo boxes, text fields and labels copy their colours at that moment
        if (mainWindow) mainWindow->sendLookAndFeelChange();
        else if (auto* m = getMainComponent()) m->sendLookAndFeelChange();
        for (juce::Component* w : { (juce::Component*) miniWindow.get(), (juce::Component*) overlayWindow.get() })
            if (w != nullptr) { w->sendLookAndFeelChange(); w->repaint(); }
        if (auto* m = getMainComponent()) m->themeChanged();
        if (mainWindow) mainWindow->setBackgroundColour (t.bg());
    };
    if (appSettings.get ("theme").toString() == "ocean-blue") appSettings.set ("theme", "cobalt-minimal");   // merged in 1.0
    if (const auto accent = appSettings.get ("accent").toString(); accent.startsWith ("#"))
        themeManager.setAccentOverride (juce::Colour::fromString ("ff" + accent.substring (1)));
    if (appSettings.get ("appearance").toString() == "system") appSettings.set ("theme", themeForAppearance());
    if (! themeManager.apply (appSettings.get ("theme", "aura").toString()))
        themeManager.apply ("aura");
    juce::Desktop::getInstance().addDarkModeSettingListener (this);
    lookAndFeel.applyTheme (themeManager.current());
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Plug-ins ("Spectre circulaire" was withdrawn: a copy installed from the old catalogue goes too)
    dataDir.getChildFile ("Plugins").getChildFile ("UI").getChildFile ("nx-radial").deleteRecursively();
    pluginManager.setSearchFolders ({ resourcesDir.getChildFile ("Plugins"), dataDir.getChildFile ("Plugins") });
    if (auto* ui = appSettings.get ("uiPlugins").getArray())
        for (auto& p : pluginManager.getUiPlugins()) pluginManager.setUiPluginEnabled (p.id, ui->contains (p.id));
    if (auto* dsp = appSettings.get ("dspPlugins").getArray())
        for (auto& p : pluginManager.getDspPlugins()) pluginManager.setDspPluginEnabled (p.id, dsp->contains (p.id));
    pluginManager.onLayoutRequest = [this] (const juce::var& l)
    {
        if (auto* o = l.getDynamicObject())
            for (auto& prop : o->getProperties()) layout.getDynamicObject()->setProperty (prop.name, prop.value);
        if (auto* m = getMainComponent()) { m->resized(); if (m->isImmersive()) m->getImmersive().refreshConfig(); }
    };
    pluginManager.onPluginsChanged = [this]
    {
        juce::Array<juce::var> ui, dsp;
        for (auto& p : pluginManager.getUiPlugins()) if (p.enabled) ui.add (p.id);
        for (auto& p : pluginManager.getDspPlugins()) if (p.enabled) dsp.add (p.id);
        appSettings.set ("uiPlugins", ui);
        appSettings.set ("dspPlugins", dsp);
        rebuildDspChain();
        if (auto* m = getMainComponent()) m->pluginsChanged();
    };
    {
        // headphone EQ chosen in AURA DSP (kept across launches)
        const auto eq = appSettings.get ("headphoneEq");
        int index = -1;
        for (int i = 0; i < (int) hpeq::presets().size(); ++i)
            if (eq.getProperty ("preset", {}).toString() == hpeq::presets()[(size_t) i].id) index = i;
        studio->autoHeadroom = (bool) eq.getProperty ("autoHeadroom", true);
        studio->setEq (index, (bool) eq.getProperty ("bypass", false));
        const auto peq = appSettings.get ("parametricEq");
        studio->setParametric (bandsFromVar (peq.getProperty ("bands", {})), (bool) peq.getProperty ("enabled", false));
        const auto e9 = appSettings.get ("e9ds");
        e9dsState->set ((bool) e9.getProperty ("enabled", false), (double) e9.getProperty ("focus", 6500.0),
                        (double) e9.getProperty ("amount", 6.0), (int) e9.getProperty ("mode", 0));
    }
    if (studio->anyActive() || e9dsState->enabled.load()) applyStudioTools(); else rebuildDspChain();

    // Engine notifications (message thread)
    eng->onTrackChanged = [this] { onTrackChanged(); };
    eng->onStateChanged = [this] { if (auto* m = getMainComponent()) m->stateChanged(); updateSystemNowPlaying (true); };
    eng->onSignalPathChanged = [this] { if (auto* m = getMainComponent()) m->stateChanged(); };
    eng->setShuffle ((bool) appSettings.get ("shuffle", false));
    eng->setRepeat ((PlaybackEngine::Repeat) juce::jlimit (0, 2, (int) appSettings.get ("repeat", 0)));
    eng->onError = [this] (const juce::String& e)
    {
        if (e.startsWith (PlaybackEngine::studioMasterError)) showUpsell (UpsellReason::StudioMaster);   // Aura Free
        else showMessage (e);
    };

    lib.onFavouriteChanged = [this] (juce::int64 id, bool fav)
    {
        for (auto& q : queueTracks) if (q.id == id) q.favourite = fav;
        if (auto* m = getMainComponent())
        {
            m->player.syncTransportButtons();
            m->favouriteChanged();          // hearts of Now Playing and the immersive view
        }
    };
    lib.onChanged = [this]
    {
        if (auto* m = getMainComponent())
        {
            const auto page = m->getCurrentPage().page;
            if (page != PageId::NowPlaying && page != PageId::Settings) m->refreshCurrentPage();
            else m->sidebar.refresh();
        }
    };

    if (options.createWindow)
    {
        mainWindow = std::make_unique<MainWindow> (*this, lookAndFeel);
        libScanner->start (3000);

        // Media keys and the system's "Now Playing" (works with Aura in the background)
        MediaControls::Callbacks cb;
        cb.play = [this] { eng->play(); };
        cb.pause = [this] { eng->pause(); };
        cb.toggle = [this] { eng->togglePlayPause(); };
        cb.next = [this] { eng->next(); };
        cb.previous = [this] { eng->previous(); };
        cb.seek = [this] (double t) { eng->seek (t); updateSystemNowPlaying (true); };
        void* handle = mainWindow->getPeer() != nullptr ? mainWindow->getPeer()->getNativeHandle() : nullptr;
        mediaControls = MediaControls::create (std::move (cb), handle);

        // NAS: no network scan at startup (macOS would ask for the local network permission before the user
        // even looks at the NAS). Only the servers already configured are reconnected; discovery runs when the
        // NAS page is opened.
        juce::StringArray known;
        if (auto* arr = appSettings.get ("nasServers").getArray()) for (auto& v : *arr) known.add (v.toString());
        if (auto* upnp = dynamic_cast<UpnpProvider*> (unified->getProvider ("upnp")); upnp != nullptr && ! known.isEmpty())
        {
            std::weak_ptr<int> weak = alive;
            nasDiscoveryRunning = true;
            juce::Thread::launch ([this, upnp, known, weak]
            {
                for (auto& loc : known) upnp->addServerFromLocation (loc);
                nasDiscoveryRunning = false;
                juce::MessageManager::callAsync ([this, weak]
                {
                    if (weak.expired()) return;
                    saveNasServers();
                    if (auto* m = getMainComponent()) m->sidebar.refresh();
                });
            });
        }
    }
    else
    {
        headlessMain = std::make_unique<MainComponent> (*this);
        headlessMain->setSize (1360, 860);
    }

    // First launch: the setup assistant (language, layout, audio device, music sources)
    if (options.createWindow && ! (bool) appSettings.get ("onboardingDone", false))
    {
        std::weak_ptr<int> weak = alive;
        juce::MessageManager::callAsync ([this, weak] { if (! weak.expired()) showOnboarding(); });
    }

    lastFrame = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if ((bool) appSettings.get ("remoteEnabled", false) && isPremium()) setRemoteEnabled (true);   // left on: the saved link works again at once
    if (options.createWindow && (bool) appSettings.get ("nowPlayingOverlay", false)) setNowPlayingOverlay (true);
    if (options.createWindow && (bool) appSettings.get ("autoUpdateCheck", true))
    {
        // 3 s after the launch, in the background: only an available update is announced
        std::weak_ptr<int> weak = alive;
        juce::Timer::callAfterDelay (3000, [this, weak] { if (! weak.expired()) checkForUpdates (false); });
    }
    startTimerHz (30);
}

//==============================================================================
bool AppController::isPremium() { return licenseManager != nullptr && licenseManager->isPremium(); }

void AppController::applyEdition()
{
    auto st = eng->getSettings();
    st.freeEdition = ! isPremium();
    eng->setSettings (st);
    if (! isPremium() && remoteServer != nullptr && remoteServer->isRunning()) remoteServer->stop();
    if (auto* m = getMainComponent()) { m->sidebar.repaint(); m->refreshCurrentPage(); m->stateChanged(); }
}

bool AppController::activateLicense (const juce::String& key, juce::String& error)
{
    if (! licenseManager->activate (key, error)) return false;
    appSettings.set ("licenseKey", licenseManager->getKey());
    applyEdition();
    showMessage ("Aura Premium activé — merci !"_t);
    return true;
}

void AppController::deactivateLicense()
{
    licenseManager->deactivate();
    appSettings.set ("licenseKey", juce::String());
    applyEdition();
    showMessage ("Licence retirée de cet appareil : Aura Free"_t);
}

juce::String AppController::upsellTitle (UpsellReason why)
{
    switch (why)
    {
        case UpsellReason::StudioMaster:   return "Format Studio Master (DXD / DSD / SACD)"_t;
        case UpsellReason::ExclusiveMode:  return "Mode exclusif (Hog Mode) — Aura Premium"_t;
        case UpsellReason::DeviceProfiles: return "Profils matériels dédiés — Aura Premium"_t;
        case UpsellReason::Remote:         return "Aura Remote — Aura Premium"_t;
        case UpsellReason::MetadataEditor: return "Éditeur de métadonnées — Aura Premium"_t;
    }
    return {};
}

juce::String AppController::upsellMessage (UpsellReason why)
{
    if (why == UpsellReason::StudioMaster)
        return "Passez à Aura Premium pour débloquer la lecture haute résolution au-delà de 192 kHz."_t;
    return "Cette fonction fait partie d'Aura Premium. L'édition Free lit tout le PCM jusqu'à 24 bits / 192 kHz."_t;
}

void AppController::showUpsell (UpsellReason why)
{
    ++upsellCount;
    lastUpsell = why;
    if (upsellOpen) return;
    upsellOpen = true;
    auto* w = new juce::AlertWindow (upsellTitle (why), upsellMessage (why), juce::MessageBoxIconType::NoIcon);
    w->addButton ("Entrer une clé de licence"_t, 1);
    w->addButton ("Acheter Aura Premium"_t, 2);
    w->addButton ("Plus tard"_t, 0, juce::KeyPress (juce::KeyPress::escapeKey));
    std::weak_ptr<int> weak = alive;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, weak] (int result)
    {
        if (weak.expired()) return;
        upsellOpen = false;
        if (result == 1) navigate ({ PageId::Settings, "apropos" });
        if (result == 2) openShop();
    }), true);
}

void AppController::checkForUpdates (bool userAsked)
{
    std::weak_ptr<int> weak = alive;
    updateChecker->checkNow ([this, weak, userAsked] (const UpdateChecker::Status& s)
    {
        if (weak.expired()) return;
        if (s.state == UpdateChecker::State::Available)
            showUpdateAvailable (s.info, userAsked);
        else if (userAsked && s.state == UpdateChecker::State::UpToDate)
            showMessage ("Aura est à jour (v"_t + juce::String (NX_VERSION) + ")");
        else if (userAsked)
            showMessage ("Vérification des mises à jour impossible : "_t + s.error);
        if (auto* m = getMainComponent()) m->refreshCurrentPage();
    });
}

juce::String AppController::updateMessage (const UpdateInfo& info, const juce::String& currentVersion, const juce::String& language)
{
    const auto notes = (language.startsWith ("en") && info.releaseNotesEn.isNotEmpty() ? info.releaseNotesEn : info.releaseNotes).trim();
    juce::String m;
    m << "Version installée : "_t << currentVersion << "\n" << "Nouvelle version : "_t << info.latestVersion << "\n";
    if (notes.isNotEmpty()) m << "\n" << "Nouveautés :"_t << "\n" << notes << "\n";
    if (info.mandatory) m << "\n" << "Cette mise à jour est nécessaire pour continuer à utiliser Aura."_t << "\n";
    m << "\n" << "Votre licence et vos réglages sont conservés."_t;
    return m;
}

void AppController::showUpdateAvailable (const UpdateInfo& info, bool userAsked)
{
    // "Ignorer cette version": no automatic popup for this version any more (unless mandatory or asked from the settings)
    if (! userAsked && ! info.mandatory && appSettings.get ("skippedVersion").toString() == info.latestVersion) return;
    if (updateOpen) return;
    updateOpen = true;
    auto* w = new juce::AlertWindow ("Mise à jour disponible — Aura "_t + info.latestVersion,
                                     updateMessage (info, NX_VERSION, i18n::getLanguage()), juce::MessageBoxIconType::NoIcon);
    w->addButton ("Mettre à jour"_t, 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (info.mandatory ? "Plus tard"_t : "Pas maintenant"_t, 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (! info.mandatory) w->addButton ("Ignorer cette version"_t, 2);
    std::weak_ptr<int> weak = alive;
    const auto url = info.downloadUrlForThisOS(), version = info.latestVersion;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, weak, url, version] (int result)
    {
        if (weak.expired()) return;
        updateOpen = false;
        if (result == 1 && url.isNotEmpty()) juce::URL (url).launchInDefaultBrowser();
        if (result == 2) appSettings.set ("skippedVersion", version);
    }), true);
}

std::vector<TrackInfo> AppController::withoutStudioMasters (const std::vector<TrackInfo>& tracks, int& startIndex)
{
    if (isPremium()) return tracks;
    std::vector<TrackInfo> out;
    int newStart = 0;
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        const auto& t = tracks[(size_t) i];
        if (LicenseManager::isStudioMaster (t.isDsd, t.sampleRate, t.uri)) continue;
        if (i <= startIndex) newStart = (int) out.size();
        out.push_back (t);
    }
    startIndex = juce::jlimit (0, juce::jmax (0, (int) out.size() - 1), newStart);
    return out;
}

AppController::~AppController()
{
    alive.reset();
    stopTimer();
    juce::Desktop::getInstance().removeDarkModeSettingListener (this);
    remoteServer->stop();
    sleepBlocker.setActive (false);
    mediaControls.reset();
    for (int i = 0; i < 400 && nasDiscoveryRunning.load(); ++i) juce::Thread::sleep (10);   // the NAS thread uses the providers
    if (mainWindow) appSettings.set ("windowBounds", mainWindow->getWindowStateAsString());
    eng->stop();
    miniWindow.reset();
    mainWindow.reset();
    headlessMain.reset();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    libScanner.reset();
    // The engine's DSP chains call into plug-in libraries: release them before the plug-in manager unloads those.
    audioAnalyzer.reset();
    eng.reset();
    downloadManager.reset();
    artistInfoService.reset();
}

MainComponent* AppController::getMainComponent()
{
    if (mainWindow) return &mainWindow->getMain();
    return headlessMain.get();
}

//==============================================================================
void AppController::loadEngineSettings()
{
    EngineSettings s;
    const auto v = appSettings.get ("engine");
    if (v.isObject())
    {
        s.dsdMode = (DsdMode) (int) v.getProperty ("dsdMode", 0);
        s.dopAllowed = (bool) v.getProperty ("dop", true);
        s.upsampleToDxd = (bool) v.getProperty ("upsample", false);
        s.dsdToPcmGainDb = (double) v.getProperty ("dsdGain", 0.0);
        s.exclusive = (bool) v.getProperty ("exclusive", true);
        s.bufferMilliseconds = (int) v.getProperty ("bufferMs", 0);
        s.hardwareProfile = (HardwareProfile) juce::jlimit (0, 4, (int) v.getProperty ("hardwareProfile", 0));
        s.fiioProfile = juce::jlimit (0, 1, (int) v.getProperty ("fiioProfile", 0));
        s.deviceModel = canonicalModelId (v.getProperty ("deviceModel", {}).toString());   // old RME ids -> "rme-adi2"
        s.rmeProfile = (RmeProfile) juce::jlimit (0, 1, (int) v.getProperty ("rmeProfile", 0));
        s.rmeLockVolume = (bool) v.getProperty ("rmeLockVolume", true);
        s.anubisProfile = (AnubisProfile) juce::jlimit (0, 2, (int) v.getProperty ("anubisProfile", 0));
        s.anubisDsdGainDb = (double) v.getProperty ("anubisDsdGain", 6.0);
        s.anubisHarmonics = (bool) v.getProperty ("anubisHarmonics", true);
        s.memoryPlay = (bool) v.getProperty ("memoryPlay", true);
        s.memoryPlayMaxMb = juce::jlimit (64, 8192, (int) v.getProperty ("memoryPlayMaxMb", 1536));
        s.channelMode = (ChannelMode) juce::jlimit (0, 2, (int) v.getProperty ("channelMode", 0));
        s.pcmToDsd = (int) v.getProperty ("pcmToDsd", 0);
        if (s.pcmToDsd != 64 && s.pcmToDsd != 128 && s.pcmToDsd != 256 && s.pcmToDsd != EngineSettings::xzAutomatic) s.pcmToDsd = 0;
        s.sdmOrder = (int) v.getProperty ("sdmOrder", 7) <= 5 ? 5 : 7;
        s.pcmToDsdHeadroomDb = juce::jlimit (-12.0, 0.0, (double) v.getProperty ("pcmToDsdHeadroom", -3.0));
        s.keepDsdOutput = (bool) v.getProperty ("keepDsdOutput", true);
        s.rateMode = (RateMode) juce::jlimit (0, 2, (int) v.getProperty ("rateMode", 0));
        s.transitionFades = (bool) v.getProperty ("transitionFades", true);
        if (const auto* map = v.getProperty ("channelMap", {}).getArray(); map != nullptr && map->size() == 8)
            for (int c = 0; c < 8; ++c) s.channelMap[(size_t) c] = juce::jlimit (0, 7, (int) (*map)[c]);
    }
    eng->setSettings (s);
    const auto device = appSettings.get ("device").toString();
    if (device.isNotEmpty() && eng->getBackend().getDeviceNames().contains (device)) eng->setDevice (device);
}

void AppController::saveEngineSettings()
{
    const auto s = eng->getSettings();
    auto* o = new juce::DynamicObject();
    o->setProperty ("dsdMode", (int) s.dsdMode);
    o->setProperty ("channelMode", (int) s.channelMode);
    o->setProperty ("pcmToDsd", s.pcmToDsd);
    o->setProperty ("sdmOrder", s.sdmOrder);
    o->setProperty ("pcmToDsdHeadroom", s.pcmToDsdHeadroomDb);
    o->setProperty ("keepDsdOutput", s.keepDsdOutput);
    o->setProperty ("rateMode", (int) s.rateMode);
    o->setProperty ("transitionFades", s.transitionFades);
    juce::Array<juce::var> map;
    for (auto m : s.channelMap) map.add (m);
    o->setProperty ("channelMap", map);
    o->setProperty ("dop", s.dopAllowed);
    o->setProperty ("upsample", s.upsampleToDxd);
    o->setProperty ("dsdGain", s.dsdToPcmGainDb);
    o->setProperty ("exclusive", s.exclusive);
    o->setProperty ("bufferMs", s.bufferMilliseconds);
    o->setProperty ("hardwareProfile", (int) s.hardwareProfile);
    o->setProperty ("anubisProfile", (int) s.anubisProfile);
    o->setProperty ("anubisDsdGain", s.anubisDsdGainDb);
    o->setProperty ("anubisHarmonics", s.anubisHarmonics);
    o->setProperty ("rmeProfile", (int) s.rmeProfile);
    o->setProperty ("fiioProfile", s.fiioProfile);
    o->setProperty ("deviceModel", s.deviceModel);
    o->setProperty ("rmeLockVolume", s.rmeLockVolume);
    o->setProperty ("memoryPlay", s.memoryPlay);
    o->setProperty ("memoryPlayMaxMb", s.memoryPlayMaxMb);
    appSettings.set ("engine", juce::var (o));
}

void AppController::applyEngineSettings()
{
    saveEngineSettings();
    if (auto* m = getMainComponent()) m->stateChanged();
}

juce::StringArray AppController::getBackendNames()
{
    juce::StringArray names;
    for (auto& b : createPlatformBackends()) names.add (b->getTypeName());
    if (names.isEmpty()) names.add (backendName);
    return names;
}

juce::String AppController::getCurrentBackendName() { return eng->getBackend().getTypeName(); }

void AppController::selectBackend (const juce::String& name)
{
    for (auto& b : createPlatformBackends())
        if (b->getTypeName() == name)
        {
            eng->setBackend (std::move (b));
            backendName = name;
            appSettings.set ("backend", name);
            appSettings.set ("device", eng->getDeviceName());
            showMessage ("Sortie : "_t + name + " · "_t + eng->getDeviceName());
            return;
        }
}

void AppController::selectDevice (const juce::String& name)
{
    eng->setDevice (name);
    appSettings.set ("device", name);
    showMessage ("Périphérique : "_t + name);
}

void AppController::saveNasServers()
{
    if (auto* upnp = dynamic_cast<UpnpProvider*> (unified->getProvider ("upnp")))
    {
        juce::Array<juce::var> list;
        for (auto& loc : upnp->getKnownLocations()) list.add (loc);
        if (! list.isEmpty()) appSettings.set ("nasServers", list);
    }
}

void AppController::applyThemeNow (juce::String id)
{
    if (themeManager.apply (id)) appSettings.set ("theme", id);
}

void AppController::applyTheme (juce::String id)
{
    const auto* th = themeManager.findTheme (id);
    if (th == nullptr) return;
    // a theme picked by hand becomes the dark (or light) theme of the appearance
    const bool light = th->bg().getPerceivedBrightness() > 0.6f;
    appSettings.set (light ? "lightTheme" : "darkTheme", id);
    if (getAppearance() != "system") appSettings.set ("appearance", light ? "light" : "dark");
    applyThemeNow (id);                                    // hot: every component follows at once (no restart)
}

void AppContext::chooseThemeToImport()
{
    auto chooser = std::make_shared<juce::FileChooser> ("Importer un thème Aura"_t, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"),
                                                        "*.json;*.zip;*.nxtheme");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == juce::File()) return;
        juce::String err;
        const auto id = importThemeFile (f, err);
        if (id.isEmpty()) showMessage ("Thème non importé : "_t + err);
        else showMessage ("Thème importé et appliqué : "_t + theme().name);
    });
}

juce::String AppContext::diagnosticsSummary()
{
    auto& e = engine();
    const auto path = e.getSignalPath();
    juce::String s;
    s << "Aura " NX_VERSION " (" << (isPremium() ? "Premium" : "Free") << ")\n"
      << "Système : " << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " 64 bits" : "") << "\n"
      << "Processeur : " << juce::SystemStats::getCpuModel().trim() << " · " << juce::SystemStats::getNumCpus() << " cœurs\n"
      << "Mémoire : " << juce::SystemStats::getMemorySizeInMegabytes() << " Mo\n"
      << "Langue : " << (i18n::isEnglish() ? "en" : "fr") << " · thème : " << theme().id << " · mode Streamer : " << (isStreamerMode() ? "oui" : "non") << "\n"
      << "Tampon : " << e.getOutputStatus().bufferFrames << " trames à " << e.getOutputStatus().sampleRate << " Hz"
      << (e.isOnSharedFallback() ? " · repli sortie partagée" : "") << "\n"
      << "Sortie : " << e.getDeviceName() << " · chaîne : " << (path.nodes.empty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")) : toString (path.overall)) << "\n"
      << "Généré le : " << juce::Time::getCurrentTime().toISO8601 (true) << "\n";
    return s;
}

void AppContext::exportDiagnostics()
{
    const auto zip = diagnostics::exportZip (diagnosticsSummary(), juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    if (zip == juce::File()) { showMessage ("Impossible d'écrire les journaux sur le bureau"_t); return; }
    zip.revealToUser();
    showMessage ("Journaux exportés sur le bureau : "_t + zip.getFileName() + " — envoyez ce fichier au support"_t);
}

void AppContext::chooseThemeExport()
{
    const auto name = juce::File::createLegalFileName (theme().id.replaceCharacter ('-', '_')) + ".json";
    auto chooser = std::make_shared<juce::FileChooser> ("Exporter le thème actuel"_t, juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (name), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (! f.hasFileExtension ("json")) f = f.withFileExtension ("json");
        if (exportCurrentTheme (f)) showMessage ("Thème exporté : "_t + f.getFileName());
        else showMessage ("Export impossible : "_t + f.getFullPathName());
    });
}

juce::String AppController::importThemeFile (const juce::File& file, juce::String& error)
{
    const auto id = themeManager.importTheme (file, error);
    if (id.isEmpty()) return {};
    applyTheme (id);                         // Aura Remote follows through its state (theme colours)
    if (auto* m = getMainComponent()) m->pluginsChanged();     // the Extensions cards
    return id;
}

bool AppController::exportCurrentTheme (const juce::File& destination)
{
    return ThemeManager::exportThemeJson (themeManager.current(), destination);
}

juce::String AppController::themeForAppearance()
{
    const auto mode = getAppearance();
    const bool dark = mode == "dark" || (mode == "system" && juce::Desktop::getInstance().isDarkModeActive());
    auto id = appSettings.get (dark ? "darkTheme" : "lightTheme", dark ? "aura" : lightThemeId).toString();
    if (themeManager.findTheme (id) == nullptr) id = dark ? "aura" : lightThemeId;
    return id;
}

void AppController::setAppearance (const juce::String& mode)
{
    if (mode != "dark" && mode != "light" && mode != "system") return;
    // the current dark theme is kept as the one to come back to
    if (const auto cur = themeManager.current(); cur.bg().getPerceivedBrightness() <= 0.6f && appSettings.get ("darkTheme").isVoid())
        appSettings.set ("darkTheme", cur.id);
    appSettings.set ("appearance", mode);
    applyThemeNow (themeForAppearance());
}

void AppController::setAccent (juce::Colour accent)
{
    appSettings.set ("accent", accent.isTransparent() ? juce::String() : "#" + accent.toDisplayString (false));
    themeManager.setAccentOverride (accent);
}

void AppController::restartApplication()
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getFullPathName();
    // Single-instance app: a helper waits for this process to exit, then starts the new one.
   #if JUCE_WINDOWS
    const juce::StringArray cmd { "cmd", "/c", "ping", "-n", "3", "127.0.0.1", ">nul", "&", "start", "\"\"", exe };
   #elif JUCE_MAC
    const juce::StringArray cmd { "/bin/sh", "-c", "sleep 1; open -n \"$0\"", exe };
   #else
    const juce::StringArray cmd { "/bin/sh", "-c", "sleep 1; exec \"$0\"", exe };
   #endif
    static juce::ChildProcess helper;
    if (helper.start (cmd)) juce::JUCEApplication::getInstance()->systemRequestedQuit();
    else showMessage ("Redémarrage automatique impossible : relancez Aura manuellement."_t);
}

//==============================================================================
void AppController::playTracks (const std::vector<TrackInfo>& requested, int startIndex)
{
    if (requested.empty()) return;
    startIndex = juce::jlimit (0, (int) requested.size() - 1, startIndex);
    if (! isPremium())
    {
        // Aura Free: a Studio Master track asked for -> the Premium window; others in the list are skipped
        const auto& wanted = requested[(size_t) startIndex];
        if (LicenseManager::isStudioMaster (wanted.isDsd, wanted.sampleRate, wanted.uri)) { showUpsell (UpsellReason::StudioMaster); return; }
    }
    const auto tracks = withoutStudioMasters (requested, startIndex);
    if (tracks.empty()) return;
    const auto& chosen = tracks[(size_t) startIndex];

    if (unified->routeFor (chosen) == PlaybackRoute::External)
    {
        juce::String err;
        if (auto* p = unified->getProvider (chosen.source))
        {
            if (p->playExternally (chosen, err))
            {
                eng->pause();
                externalTrack = chosen;
                externalPlaying = true;
                onTrackChanged();
                showMessage (p->lastError.isNotEmpty() ? p->lastError
                                                       : "Lecture confiée à "_t + p->getDisplayName() + " (hors moteur bit-perfect)");
            }
            else showMessage (p->getDisplayName() + " : " + err);
        }
        return;
    }

    externalPlaying = false;
    shuffleScope = {};                                   // a hand-picked list ends any endless shuffle
    queueTracks.clear();
    std::vector<PlayableItem> items;
    int start = 0;
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        const auto& t = tracks[(size_t) i];
        if (unified->routeFor (t) != PlaybackRoute::Engine) continue;
        if (i == startIndex) start = (int) items.size();
        queueTracks.push_back (t);
        items.push_back (unified->toPlayable (t));
    }
    eng->setQueue (std::move (items), start, true);
}

std::vector<TrackInfo> AppController::collectionTracks (const juce::String& scope)
{
    int ignored = 0;
    return withoutStudioMasters (scope == "favourites" ? lib.getFavouriteTracks() : lib.getTracks ("artist", 1000000), ignored);
}

void AppController::playCollection (const juce::String& scope, bool shuffle)
{
    auto tracks = collectionTracks (scope);
    if (tracks.empty()) { showMessage ("Aucun morceau à lire"_t); return; }
    if (! shuffle) { playTracks (tracks, 0); return; }

    shufflePass = std::move (tracks);
    std::shuffle (shufflePass.begin(), shufflePass.end(), rng);
    shuffleNext = 0;
    lastQueuedKey = 0;
    const size_t first = std::min<size_t> (shufflePass.size(), 200);
    playTracks (std::vector<TrackInfo> (shufflePass.begin(), shufflePass.begin() + (std::ptrdiff_t) first), 0);
    setShuffle (true);                                    // the button follows: the whole collection is being shuffled
    shuffleNext = first;
    shuffleScope = scope;                                 // after playTracks, which clears it
    if (! queueTracks.empty()) lastQueuedKey = queueTracks.back().id;
}

void AppController::extendShuffle()
{
    // Called on every track change: keep at least 20 tracks queued after the current one.
    if (shuffleScope.isEmpty() || externalPlaying) return;
    const int remaining = eng->getRemainingCount();
    if (remaining >= 20) return;

    std::vector<TrackInfo> chunk;
    while (chunk.size() < 200)
    {
        if (shuffleNext >= shufflePass.size())
        {
            // new pass over the (possibly changed) collection; its first track must differ from the last one queued
            shufflePass = collectionTracks (shuffleScope);
            if (shufflePass.empty()) break;
            std::shuffle (shufflePass.begin(), shufflePass.end(), rng);
            const auto last = chunk.empty() ? lastQueuedKey : chunk.back().id;
            if (shufflePass.size() > 1 && shufflePass.front().id == last) std::swap (shufflePass.front(), shufflePass.back());
            shuffleNext = 0;
        }
        chunk.push_back (shufflePass[shuffleNext++]);
        if (shufflePass.size() == 1 && chunk.size() >= 1) break;     // a single track: repeat it, one per call
    }
    if (chunk.empty()) return;
    lastQueuedKey = chunk.back().id;

    std::vector<PlayableItem> items;
    for (auto& t : chunk)
        if (unified->routeFor (t) == PlaybackRoute::Engine) { queueTracks.push_back (t); items.push_back (unified->toPlayable (t)); }
    if (! items.empty()) eng->appendToQueue (std::move (items));
}

void AppController::enqueue (const std::vector<TrackInfo>& requested)
{
    int ignored = 0;
    const auto tracks = withoutStudioMasters (requested, ignored);
    if (tracks.size() < requested.size()) showUpsell (UpsellReason::StudioMaster);
    if (tracks.empty()) return;
    std::vector<PlayableItem> items;
    for (auto& t : tracks)
        if (unified->routeFor (t) == PlaybackRoute::Engine) { queueTracks.push_back (t); items.push_back (unified->toPlayable (t)); }
    if (eng->getQueue().empty()) { playTracks (tracks, 0); return; }
    eng->appendToQueue (std::move (items));
    showMessage (juce::String ((int) tracks.size()) + " morceau(x) ajouté(s) à la file"_t);
}

const TrackInfo* AppController::currentTrack()
{
    if (externalPlaying) return &externalTrack;
    const int i = eng->getCurrentIndex();
    return i >= 0 && i < (int) queueTracks.size() ? &queueTracks[(size_t) i] : nullptr;
}

bool AppController::toggleCurrentFavourite()
{
    const auto* t = currentTrack();
    if (t == nullptr || externalPlaying || t->id <= 0) { showMessage ("Ce morceau n'est pas dans la bibliothèque"_t); return false; }
    const bool fav = ! t->favourite;
    lib.setFavourite (t->id, fav);          // onFavouriteChanged updates the queue copy and the heart
    lib.notifyChanged();                    // the visible lists (and the Favourites page) show the new heart
    showMessage (fav ? "Ajouté aux favoris"_t : "Retiré des favoris"_t);
    return true;
}

void AppController::setShuffle (bool on)
{
    if (eng->isShuffle() == on) return;
    eng->setShuffle (on);
    appSettings.set ("shuffle", on);
    showMessage (on ? "Lecture aléatoire activée"_t : "Lecture aléatoire désactivée"_t);
}

void AppController::setRepeat (PlaybackEngine::Repeat mode)
{
    eng->setRepeat (mode);
    appSettings.set ("repeat", (int) mode);
    showMessage (mode == PlaybackEngine::Repeat::One ? "Répétition : piste en cours"_t
                 : mode == PlaybackEngine::Repeat::All ? "Répétition : toute la file"_t : "Répétition désactivée"_t);
}

void AppController::navigate (const PageRequest& r)
{
    if (auto* m = getMainComponent()) m->navigate (r);
}

void AppController::toggleMiniPlayer()
{
    if (mainWindow == nullptr) return;
    if (miniWindow == nullptr) miniWindow = std::make_unique<MiniWindow> (*this);
    const bool showMini = ! miniWindow->isVisible();
    if (showMini)
    {
        miniWindow->setTopLeftPosition (mainWindow->getScreenX() + mainWindow->getWidth() - 400, mainWindow->getScreenY() + 40);
        miniWindow->setVisible (true);
        miniWindow->getMini().trackChanged();
        mainWindow->setMinimised (true);
    }
    else
    {
        miniWindow->setVisible (false);
        mainWindow->setMinimised (false);
        mainWindow->toFront (true);
    }
}

void AppController::setNowPlayingOverlay (bool show)
{
    if (overlayWindow == nullptr)
    {
        if (! show) return;
        overlayWindow = std::make_unique<OverlayWindow> (*this);
        const auto saved = appSettings.get ("overlayPosition").toString();
        if (saved.isNotEmpty()) overlayWindow->restoreWindowStateFromString (saved);
        else if (mainWindow != nullptr) overlayWindow->setTopLeftPosition (mainWindow->getScreenX() + 40, mainWindow->getScreenY() + 60);
    }
    if (! show) appSettings.set ("overlayPosition", overlayWindow->getWindowStateAsString());
    overlayWindow->setVisible (show);
    if (show) overlayWindow->getOverlay().trackChanged();
    appSettings.set ("nowPlayingOverlay", show);
}

bool AppController::isNowPlayingOverlayVisible() { return overlayWindow != nullptr && overlayWindow->isVisible(); }

void AppController::exportNowPlaying()
{
    if (! perf::streamer()) { exportedNowPlaying = {}; return; }
    // OBS files: only when the track or its artwork changes, written on a worker thread (never here, never on the
    // audio thread). The cover comes from the artwork cache (already decoded in the background).
    const auto* t = currentTrack();
    const auto cover = t != nullptr ? artworkCache->get (t->artworkKey, 600) : juce::Image();
    const auto key = t != nullptr ? juce::String (t->id) + "|" + t->title + "|" + (cover.isValid() ? t->artworkKey : juce::String()) : juce::String ("-");
    if (key == exportedNowPlaying) return;
    exportedNowPlaying = key;
    std::optional<TrackInfo> track;
    if (t != nullptr) track = *t;
    const auto dir = dataDir;
    const auto image = cover.isValid() ? cover.createCopy() : juce::Image();
    obsWriter.removeAllJobs (false, 0);                   // only the latest track matters
    obsWriter.addJob ([dir, track, image] { writeNowPlayingFiles (dir, track ? &*track : nullptr, image); });
}

bool AppController::writeNowPlayingFiles (const juce::File& dir, const TrackInfo* t, const juce::Image& cover)
{
    juce::String line;
    if (t != nullptr) line = t->displayArtist().isNotEmpty() ? t->displayArtist() + juce::String::fromUTF8 (" \xe2\x80\x94 ") + t->title : t->title;
    bool ok = dir.getChildFile ("now_playing.txt").replaceWithText (line, false, false, "\n");
    const auto jpg = dir.getChildFile ("now_playing_cover.jpg");
    if (cover.isValid())
    {
        // write next to it, then swap: OBS never reads a half-written image
        const auto tmp = dir.getChildFile ("now_playing_cover.tmp");
        tmp.deleteFile();
        {
            juce::FileOutputStream out (tmp);
            juce::JPEGImageFormat jpeg;
            jpeg.setQuality (0.9f);
            ok = out.openedOk() && jpeg.writeImageToStream (cover.convertedToFormat (juce::Image::RGB), out) && ok;
        }
        ok = tmp.moveFileTo (jpg) && ok;
    }
    else jpg.deleteFile();
    return ok;
}

void AppController::showMessage (const juce::String& text)
{
    if (auto* m = getMainComponent()) m->showMessage (text);
    juce::Logger::writeToLog (text);
}

const juce::Image& AppController::backdrop()
{
    if (auto* m = getMainComponent()) return m->background.getGlassBackdrop();
    return emptyImage;
}

juce::Component* AppController::backdropOwner() { return getMainComponent(); }

void AppController::openFiles (const juce::StringArray& paths)
{
    std::vector<TrackInfo> tracks;
    for (auto& p : paths)
    {
        juce::File f (p);
        TrackInfo t;
        if (f.hasFileExtension ("iso"))
            for (auto tr : readSacdIso (f, lib)) { lib.upsert (tr); tracks.push_back (tr); }
        else if (f.existsAsFile() && readLocalTrack (f, lib, t)) { lib.upsert (t); tracks.push_back (t); }
    }
    if (! tracks.empty()) playTracks (tracks, 0);
}

//==============================================================================
void AppController::updateSystemNowPlaying (bool force)
{
    if (mediaControls == nullptr) return;
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const auto* t = currentTrack();
    // Without a forced update, refresh every 5 s (position drift) or while the artwork is still loading.
    const bool artworkPending = t != nullptr && t->artworkKey.isNotEmpty() && nowPlayingArtworkSent != t->artworkKey;
    if (! force && now - lastNowPlayingUpdate < 5.0 && ! (artworkPending && now - lastNowPlayingUpdate > 0.5)) return;
    lastNowPlayingUpdate = now;

    NowPlayingInfo info;
    if (t != nullptr)
    {
        info.hasTrack = true;
        info.title = t->title;
        info.artist = t->displayArtist();
        info.album = t->album;
        info.duration = t->duration;
        info.position = externalPlaying ? 0.0 : eng->getPositionSeconds();
        info.playing = eng->getState() == PlaybackEngine::State::Playing;
        info.artworkId = nowPlayingArtworkSent;
        if (artworkPending)
        {
            const auto img = artworkCache->get (t->artworkKey, 600);
            if (img.isValid())
            {
                juce::MemoryOutputStream png;
                juce::PNGImageFormat().writeImageToStream (img, png);
                info.artworkPng = png.getMemoryBlock();
                info.artworkId = nowPlayingArtworkSent = t->artworkKey;
            }
        }
    }
    mediaControls->update (info);
}

void AppController::onTrackChanged()
{
    if (mediaControls != nullptr && nowPlayingArtworkSent.isNotEmpty())
    {
        const auto* t = currentTrack();
        if (t == nullptr || t->artworkKey != nowPlayingArtworkSent) nowPlayingArtworkSent = "none:" + juce::String (juce::Random::getSystemRandom().nextInt());
    }
    updateSystemNowPlaying (true);
    extendShuffle();
    {
        // one background scan per track (cancels the previous one), read by the player bar and the immersive view
        const auto* t = currentTrack();
        analysisPath = t == nullptr || externalPlaying ? juce::String()
                     : t->source == "local" ? t->externalId : lib.getOfflineCopy (*t);
        if (analysisPath.isNotEmpty()) trackScan->request (analysisPath);
    }
    if (const auto* t = currentTrack(); t != nullptr && ! externalPlaying && t->artworkKey.isEmpty())
    {
        // no cover yet (embedded picture or folder image added after the scan, CD1/ folder...): look now, keep it
        if (const auto key = lib.resolveMissingArtwork (*t); key.isNotEmpty())
        {
            const auto id = t->id;
            for (auto& q : queueTracks) if (q.id == id && q.artworkKey.isEmpty()) q.artworkKey = key;
            lib.notifyChanged();
        }
    }
    if (auto* m = getMainComponent()) m->trackChanged();
    if (miniWindow) miniWindow->getMini().trackChanged();
    if (overlayWindow) overlayWindow->getOverlay().trackChanged();
    if (const auto* t = currentTrack())
    {
        if (t->id > 0 && ! externalPlaying) lib.markPlayed (t->id);
        loadLyrics (*t);
    }
}

void AppController::loadLyrics (const TrackInfo& t)
{
    const auto local = loadLocalLyrics (t);      // local files and tags only: no online lyrics service
    if (auto* m = getMainComponent())
        m->setLyrics (local, local.isEmpty() ? juce::String ("Pas de paroles pour ce morceau"_t) : juce::String());
}

void AppController::timerCallback()
{
    // A fixed rate: this timer serves the transport, Aura Remote and the OS media keys (never 1 Hz).
    if (const int interval = perf::streamer() ? 1000 / 15 : 1000 / 30; getTimerInterval() != interval) startTimer (interval);
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double dt = juce::jlimit (0.001, 0.2, now - lastFrame);
    lastFrame = now;
    const bool playing = eng->getState() == PlaybackEngine::State::Playing;
    const bool visualisers = ! perf::streamer();          // Streamer mode: no FFT / VU for visualisers that are off
    if (visualisers) audioAnalyzer->update (dt, playing);

    pluginManager.setState ("time", now);
    perf::setBackground (mainWindow != nullptr && ! juce::Process::isForegroundProcess());
    appActivity.setPlaying (playing);
    exportNowPlaying();
    updateSystemNowPlaying (false);
    sleepBlocker.setActive (playing && (bool) appSettings.get ("preventSleep", true));
    if (remoteServer->isRunning() && now - lastRemoteState > 0.25)
    {
        lastRemoteState = now;
        remoteServer->setState (buildRemoteState());
    }
    if (visualisers) pluginManager.setState ("audio", audioAnalyzer->toVar());
    auto* player = new juce::DynamicObject();
    if (const auto* t = currentTrack())
    {
        player->setProperty ("title", t->title);
        player->setProperty ("artist", t->displayArtist());
        player->setProperty ("album", t->album);
        player->setProperty ("duration", t->duration);
        player->setProperty ("format", t->resolutionBadge());
    }
    player->setProperty ("position", eng->getPositionSeconds());
    player->setProperty ("playing", playing);
    pluginManager.setState ("player", juce::var (player));

    const auto& th = themeManager.current();
    auto* theme = new juce::DynamicObject();
    theme->setProperty ("accent", "#" + th.accent().toDisplayString (false));
    theme->setProperty ("accent2", "#" + th.accent2().toDisplayString (false));
    theme->setProperty ("text", "#" + th.textCol().toDisplayString (false));
    theme->setProperty ("textDim", "#" + th.textDim().toDisplayString (false));
    theme->setProperty ("bg", "#" + th.bg().toDisplayString (false));
    pluginManager.setState ("theme", juce::var (theme));
}

//==============================================================================
void AppController::rebuildDspChain()
{
    // two instances of the same chain: the audio thread's (PCM output) and the decoder's (before the DSD modulator)
    for (const bool forDecoder : { false, true })
    {
        auto chain = pluginManager.buildDspChain();
        if (chain == nullptr) chain = std::make_unique<DspChain>();
        chain->processors.push_back (std::make_unique<StudioToolsProcessor> (studio));
        chain->processors.push_back (std::make_unique<E9dsProcessor> (e9dsState, ! forDecoder));
        if (forDecoder) eng->setDecoderDspChain (std::move (chain));
        else eng->setDspChain (std::move (chain));
    }
}

void AppController::applyStudioTools()
{
    rebuildDspChain();
    updateDspNeedsPcm();
}

void AppController::updateDspNeedsPcm()
{
    // DSD cannot be processed as 1-bit data: while a tool is on, DSD tracks are converted to PCM (DXD).
    auto s = eng->getSettings();
    const bool need = studio->anyActive() || e9dsState->enabled.load();
    if (s.dspNeedsPcm != need)
    {
        s.dspNeedsPcm = need;
        // Re-plan the current track when DSD is involved: DSD source (remodulated / converted / back to bit-perfect),
        // or PCM -> DSD (the processing moves to the decoder, whose lead must be refilled at once)
        const auto plan = eng->getCurrentPlan();
        const bool affectsCurrent = plan.source.isDsd() || plan.remodulates();
        eng->setSettings (s, affectsCurrent);
    }
    if (auto* m = getMainComponent()) m->stateChanged();
}

void AppController::setE9ds (bool enabled, double focusHz, double amountDb, int mode)
{
    e9dsState->set (enabled, focusHz, amountDb, mode);
    auto* o = new juce::DynamicObject();
    o->setProperty ("enabled", e9dsState->enabled.load());
    o->setProperty ("focus", e9dsState->focusHz.load());
    o->setProperty ("amount", e9dsState->amountDb.load());
    o->setProperty ("mode", e9dsState->mode.load());
    appSettings.set ("e9ds", juce::var (o));
    updateDspNeedsPcm();          // the processors read the atomics: no rebuild, so switching off fades out
}

void AppController::setHeadphoneEq (int presetIndex, bool bypass)
{
    studio->setEq (presetIndex, bypass);
    auto* o = new juce::DynamicObject();
    const auto* p = studio->eqPresetInfo();
    o->setProperty ("preset", p != nullptr ? juce::String (p->id) : juce::String());
    o->setProperty ("bypass", bypass);
    o->setProperty ("autoHeadroom", studio->autoHeadroom.load());
    appSettings.set ("headphoneEq", juce::var (o));
    applyStudioTools();
}

juce::var AppController::bandsToVar (const hpeq::ParametricBands& b)
{
    juce::Array<juce::var> list;
    for (auto& band : b)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", hpeq::typeName (band.type));
        o->setProperty ("freq", band.freq);
        o->setProperty ("gain", band.gain);
        o->setProperty ("q", band.q);
        list.add (juce::var (o));
    }
    return list;
}

hpeq::ParametricBands AppController::bandsFromVar (const juce::var& v)
{
    auto b = hpeq::flatParametric();
    if (auto* list = v.getArray())
        for (int i = 0; i < juce::jmin ((int) b.size(), list->size()); ++i)
        {
            const auto& o = list->getReference (i);
            b[(size_t) i] = hpeq::clampBand ({ hpeq::typeFromName (o.getProperty ("type", "peak").toString()), (double) o.getProperty ("freq", b[(size_t) i].freq),
                                               (double) o.getProperty ("gain", 0.0), (double) o.getProperty ("q", b[(size_t) i].q) });
        }
    // Bass and Treble keep their shelf type, Band 2-4 stay peaks
    b[0].type = hpeq::BandType::LowShelf;
    b[6].type = hpeq::BandType::HighShelf;
    for (size_t i = 2; i <= 4; ++i) b[i].type = hpeq::BandType::Peak;
    return b;
}

void AppController::setParametricEq (const hpeq::ParametricBands& bands, bool enabled, bool persist)
{
    const bool wasActive = studio->anyActive();
    studio->setParametric (bands, enabled);
    if (persist)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("enabled", enabled);
        o->setProperty ("bands", bandsToVar (studio->parametricBands()));
        appSettings.set ("parametricEq", juce::var (o));
    }
    // a band moved: the processor reads the new values by itself; the chain is rebuilt only when the EQ goes on / off
    if (wasActive != studio->anyActive()) applyStudioTools();
    else if (auto* m = getMainComponent()) m->stateChanged();
}

juce::StringArray AppController::getParametricPresetNames()
{
    juce::StringArray names;
    if (auto* o = appSettings.get ("peqPresets").getDynamicObject())
        for (auto& p : o->getProperties()) names.add (p.name.toString());
    names.sortNatural();
    return names;
}

void AppController::saveParametricPreset (const juce::String& name)
{
    const auto n = name.trim();
    if (n.isEmpty()) return;
    auto* o = new juce::DynamicObject();
    if (auto* old = appSettings.get ("peqPresets").getDynamicObject())
        for (auto& p : old->getProperties()) o->setProperty (p.name, p.value);
    o->setProperty (juce::Identifier (n), bandsToVar (studio->parametricBands()));
    appSettings.set ("peqPresets", juce::var (o));
}

bool AppController::loadParametricPreset (const juce::String& name)
{
    const auto v = appSettings.get ("peqPresets").getProperty (juce::Identifier (name.trim().isEmpty() ? juce::String ("_") : name.trim()), {});
    if (! v.isArray()) return false;
    setParametricEq (bandsFromVar (v), true, true);
    return true;
}

void AppController::showAuraDsp()
{
    if (auto* m = getMainComponent()) m->showAuraDsp();
}

void AppController::setUserVolume (double v)
{
    if (auto* m = getMainComponent()) m->player.setVolumeLevel (v);
    else appSettings.set ("volume", juce::jlimit (0.0, 1.0, v));
}

double AppController::getUserVolume() { return (double) appSettings.get ("volume", 1.0); }

bool AppController::setRemoteEnabled (bool on)
{
    if (on && ! isPremium()) { showUpsell (UpsellReason::Remote); return false; }
    appSettings.set ("remoteEnabled", on);
    if (! on) { remoteServer->stop(); return true; }
    juce::String err;
    // same port as last time when it is free (the link stays valid); otherwise a new one, saved
    if (! remoteServer->start ((int) appSettings.get ("remotePort", 0), err)) { showMessage (err); return false; }
    if (remoteServer->getPort() != (int) appSettings.get ("remotePort", 0)) appSettings.set ("remotePort", remoteServer->getPort());
    remoteServer->setState (buildRemoteState());
    return true;
}

void AppController::setLanguage (const juce::String& code)
{
    i18n::setLanguage (code);
    appSettings.set ("language", i18n::getLanguage());
    artistInfoService->language = i18n::getLanguage();
    if (auto* m = getMainComponent()) m->languageChanged();
}

void AppController::applyLayoutSettings()
{
    const auto l = appSettings.get ("layout");
    auto* o = layout.getDynamicObject();
    const auto sidebarMode = l.getProperty ("sidebar", "wide").toString();
    if (sidebarMode == "compact") o->setProperty ("sidebarWidth", 76);
    else o->removeProperty ("sidebarWidth");
    const auto player = l.getProperty ("player", "bottom").toString();
    if (player == "top") o->setProperty ("playerPosition", "top");
    else o->removeProperty ("playerPosition");
    // listening bar density: comfortable (floating card), compact (thinner), integrated (thin, flush with the edge)
    const auto density = l.getProperty ("density", "comfortable").toString();
    if (density == "compact" || density == "integrated") o->setProperty ("playerDensity", density);
    else o->removeProperty ("playerDensity");
}

void AppController::setLayoutOption (const juce::String& key, const juce::var& value)
{
    auto l = appSettings.get ("layout");
    if (! l.isObject()) l = juce::var (new juce::DynamicObject());
    l.getDynamicObject()->setProperty (key, value);
    appSettings.set ("layout", l);
    applyLayoutSettings();
    if (auto* m = getMainComponent()) { m->resized(); m->sidebar.resized(); m->repaint(); }
}

void AppController::unlockZeynTheme()
{
    if (themeManager.isZeynUnlocked())
    {
        showMessage ("Thème ZEYN déjà déverrouillé — Réglages › Apparence"_t);
        return;
    }
    appSettings.set ("zeynUnlocked", true);
    themeManager.setZeynUnlocked (true);
    showMessage ("Thème ZEYN déverrouillé — Disponible dans Réglages › Apparence"_t);
}

bool AppController::isZeynUnlocked() { return themeManager.isZeynUnlocked(); }

bool AppController::handleShortcutKey (const juce::KeyPress& k)
{
    auto* m = getMainComponent();
    if (m == nullptr) return false;
    const auto action = shortcuts::match (appSettings, k);
    if (! action) return false;
    m->performShortcut (*action);
    return true;
}

void AppController::setStreamerMode (bool on)
{
    if (on == perf::streamer() && on == (bool) appSettings.get ("streamerMode", false)) return;
    if (on)
    {
        // visual extensions off for the session, their state kept to be restored afterwards
        juce::Array<juce::var> suspended;
        for (auto& p : pluginManager.getUiPlugins()) if (p.enabled) suspended.add (p.id);
        appSettings.set ("streamerSuspendedUi", suspended);
        perf::setStreamer (true);
        for (auto& p : suspended) pluginManager.setUiPluginEnabled (p.toString(), false);
    }
    else
    {
        perf::setStreamer (false);
        if (auto* list = appSettings.get ("streamerSuspendedUi").getArray())
            for (auto& id : *list) pluginManager.setUiPluginEnabled (id.toString(), true);
        appSettings.set ("streamerSuspendedUi", juce::var());
    }
    appSettings.set ("streamerMode", on);
    if (auto* m = getMainComponent())
    {
        m->streamerButton.active = on;
        m->streamerButton.repaint();
        m->repaint();
    }
    showMessage (on ? "Mode Streamer / Gaming activé : interface au repos, audio inchangé"_t : "Mode Streamer / Gaming désactivé"_t);
}

void AppController::showOnboarding()
{
    if (auto* m = getMainComponent()) m->showOnboarding();
}

void AppController::setImmersive (bool on)
{
    if (auto* m = getMainComponent()) m->setImmersive (on);
}

void AppController::sendToAuraEngine (const std::vector<TrackInfo>& tracks)
{
    juce::Array<juce::File> files;
    for (auto& t : tracks)
    {
        if (t.source == "local" && juce::File::isAbsolutePath (t.externalId) && ! sacd::isVirtualUri (t.externalId)) files.add (juce::File (t.externalId));
        else if (auto copy = lib.getOfflineCopy (t); copy.isNotEmpty()) files.add (juce::File (copy));
    }
    juce::String err;
    if (aura::openFiles (files, appSettings.get ("auraEnginePath").toString(), err))
        showMessage ("AuraEngine X : "_t + juce::String (files.size()) + " fichier(s) envoyé(s) pour analyse / conversion"_t);
    else
        showMessage (err);
}

juce::String AppController::networkSourceLabel (const juce::String& source)
{
    if (! source.startsWith ("upnp")) return {};
    const auto udn = source.fromFirstOccurrenceOf (":", false, false);
    if (auto* upnp = dynamic_cast<UpnpProvider*> (unified->getProvider ("upnp")))
        for (auto& sv : upnp->getServers())
            if (sv.udn == udn && sv.name.containsIgnoreCase ("jellyfin")) return "Jellyfin";
    return "NAS";
}

//==============================================================================
juce::MemoryBlock AppController::remoteArtwork (const juce::String& requested, int maxSize)
{
    // Whatever the stored format, the phone gets a baseline 8-bit RGB JPEG of at most maxSize x maxSize.
    auto key = requested;
    if (key.startsWith ("track:"))
    {
        // no usable key: embedded tag, then cover / folder / front next to the file (same code as the library scan)
        key = {};
        if (auto t = lib.getTrack (requested.fromFirstOccurrenceOf (":", false, false).getLargeIntValue()))
        {
            key = lib.resolveMissingArtwork (*t);         // same search as at play time, and remembered
        }
    }
    if (key.isEmpty()) return {};
    if (key.startsWith ("http") && ! requested.startsWith ("track:"))
    {
        // never an open proxy: only the covers of the library that the app itself sent to the phone
        std::lock_guard<std::mutex> l (remoteArtLock);
        if (remoteHttpArt.count (key) == 0) return {};
    }
    const auto id = key + "@" + juce::String (maxSize);
    {
        std::lock_guard<std::mutex> l (remoteArtLock);
        for (auto& e : remoteArtCache) if (e.first == id) return e.second;
    }
    auto img = key.startsWith ("http") ? artworkCache->loadNow (key, maxSize) : imagedec::decode (lib.getArtworkFile (key));
    if (! img.isValid()) return {};
    if (juce::jmax (img.getWidth(), img.getHeight()) > maxSize)
    {
        const float f = (float) maxSize / (float) juce::jmax (img.getWidth(), img.getHeight());
        img = img.rescaled (juce::jmax (1, juce::roundToInt ((float) img.getWidth() * f)), juce::jmax (1, juce::roundToInt ((float) img.getHeight() * f)), juce::Graphics::highResamplingQuality);
    }
    auto jpg = imagedec::toStandardJpeg (img);
    if (jpg.getSize() == 0) return {};
    std::lock_guard<std::mutex> l (remoteArtLock);
    remoteArtCache.emplace_back (id, jpg);
    if (remoteArtCache.size() > 120) remoteArtCache.erase (remoteArtCache.begin());
    return jpg;
}

juce::String AppController::remoteArtKey (const juce::String& key)
{
    // Online covers (http) used to be left out, so those albums never had a picture on the phone.
    if (key.startsWith ("http"))
    {
        std::lock_guard<std::mutex> l (remoteArtLock);
        if (remoteHttpArt.size() > 5000) remoteHttpArt.clear();
        remoteHttpArt.insert (key);
    }
    return key;
}

juce::var AppController::remoteTheme (const Theme& th)
{
    // "#RRGGBB", or "#RRGGBBAA" for translucent colours (CSS reads both)
    auto hex = [] (juce::Colour c)
    {
        auto s = "#" + c.toDisplayString (false);
        if (c.getAlpha() < 255) s << juce::String::toHexString ((int) c.getAlpha()).paddedLeft ('0', 2).toUpperCase();
        return s;
    };
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", th.id);
    o->setProperty ("name", th.name);
    o->setProperty ("bg", hex (th.bg()));
    o->setProperty ("bg2", hex (th.colour ("--nx-bg-2", th.bg())));
    o->setProperty ("surface", hex (th.surface()));
    o->setProperty ("surfaceLight", hex (th.glassTint()));
    o->setProperty ("text", hex (th.textCol()));
    o->setProperty ("textMuted", hex (th.textDim()));
    o->setProperty ("accent", hex (th.accent()));
    o->setProperty ("accent2", hex (th.accent2()));
    o->setProperty ("onAccent", hex (th.colour ("--nx-on-accent", juce::Colours::black)));
    o->setProperty ("border", hex (th.border()));
    o->setProperty ("bitPerfect", hex (th.colour ("--nx-quality-bitperfect", juce::Colour (0xffb07cff))));
    o->setProperty ("isLight", th.bg().getPerceivedBrightness() > 0.6f);
    return juce::var (o);
}

juce::String AppController::buildRemoteState()
{
    auto* o = new juce::DynamicObject();
    const auto* t = currentTrack();
    if (t != nullptr)
    {
        o->setProperty ("title", t->title);
        o->setProperty ("artist", t->displayArtist());
        o->setProperty ("album", t->album);
        o->setProperty ("art", remoteArtKey (t->artworkKey));
        o->setProperty ("id", t->id);
        o->setProperty ("fav", t->favourite);
        o->setProperty ("canFav", ! externalPlaying && t->id > 0);
        o->setProperty ("artV", juce::String (t->id) + "-" + t->artworkKey.substring (0, 12));   // cache-buster: changes with the track / cover
        o->setProperty ("format", t->resolutionBadge());
        o->setProperty ("duration", t->duration > 0 ? t->duration : eng->getDurationSeconds());
    }
    o->setProperty ("position", eng->getPositionSeconds());
    o->setProperty ("playing", eng->getState() == PlaybackEngine::State::Playing);
    o->setProperty ("volume", getUserVolume());
    o->setProperty ("volumeLocked", eng->isVolumeLockedByProfile());
    {
        // same rule as the player bar: DoP is a bit-exact carrier, the volume is held at 100 %
        const bool dop = t != nullptr && ! eng->getSignalPath().nodes.empty() && eng->getCurrentPlan().wire == WireFormat::DoP;
        o->setProperty ("dop", dop);
        // not isFixedVolume(): that is also "100 % = bit-perfect", which locked the phone slider at 100 % in PCM / DXD
        o->setProperty ("volumeFixed", dop || eng->isVolumeLockedByProfile());
    }
    o->setProperty ("loop", eng->isLoopActive());
    o->setProperty ("loopA", eng->getLoopA());
    o->setProperty ("loopB", eng->getLoopB());
    if (auto* m = getMainComponent()) o->setProperty ("loopPendingA", m->player.getPendingLoopA());   // A set, waiting for B
    const auto path = eng->getSignalPath();
    o->setProperty ("path", path.nodes.empty() ? juce::String() : pipelineLabel (eng->getCurrentPlan(), toString (path.overall)));
    o->setProperty ("quality", path.nodes.empty() ? juce::String() : toString (path.overall));
    const auto settings = eng->getSettings();
    const auto family = activeFamily (settings, eng->getDeviceName());
    o->setProperty ("device", eng->getDeviceName());
    o->setProperty ("family", family == DeviceFamily::Merging ? "merging" : family == DeviceFamily::Rme ? "rme" : family == DeviceFamily::Fiio ? "fiio" : "");
    juce::Array<juce::var> profiles;
    for (int i = 0; i < familyProfileCount (family); ++i) profiles.add (profileName (family, i));
    o->setProperty ("profiles", profiles);
    o->setProperty ("profile", family == DeviceFamily::None ? -1 : familyCurrentProfile (settings, family));
    o->setProperty ("studio", studio->anyActive() ? studio->describe() : juce::String());
    o->setProperty ("theme", remoteTheme (theme()));
    return juce::JSON::toString (juce::var (o), true);
}

juce::String AppController::buildRemoteLibrary (const juce::StringPairArray& params)
{
    auto* o = new juce::DynamicObject();
    const auto q = params["q"].trim();
    if (q.isEmpty())
    {
        juce::Array<juce::var> albums;
        for (auto& a : lib.getAlbums ("artist", 400))
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("title", a.title);
            x->setProperty ("artist", a.artist);
            x->setProperty ("art", remoteArtKey (a.artworkKey));
            x->setProperty ("badge", a.badge());
            albums.add (juce::var (x));
        }
        o->setProperty ("albums", albums);
    }
    else
    {
        juce::Array<juce::var> tracks;
        for (auto& tr : lib.search (q, 80))
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("id", tr.id);
            x->setProperty ("title", tr.title);
            x->setProperty ("artist", tr.displayArtist());
            x->setProperty ("album", tr.album);
            x->setProperty ("badge", tr.resolutionBadge());
            x->setProperty ("art", remoteArtKey (tr.artworkKey));
            tracks.add (juce::var (x));
        }
        o->setProperty ("tracks", tracks);
    }
    return juce::JSON::toString (juce::var (o), true);
}

void AppController::handleRemoteCommand (const juce::String& c, const juce::StringPairArray& p)
{
    auto* main = getMainComponent();
    if (c == "play") eng->play();
    else if (c == "pause") eng->pause();
    else if (c == "toggle") eng->togglePlayPause();
    else if (c == "next") eng->next();
    else if (c == "fav") toggleCurrentFavourite();
    else if (c == "prev") eng->previous();
    else if (c == "seek") eng->seek (juce::jmax (0.0, p["t"].getDoubleValue()));
    else if (c == "volume") setUserVolume (juce::jlimit (0.0, 1.0, p["v"].getDoubleValue()));
    else if (c == "loopA" && main != nullptr) main->player.markLoopA();
    else if (c == "loopB" && main != nullptr) main->player.markLoopB();
    else if (c == "loopClear" && main != nullptr) main->player.clearLoop();
    else if (c == "profile")
    {
        auto s = eng->getSettings();
        const auto family = activeFamily (s, eng->getDeviceName());
        const int i = p["i"].getIntValue();
        if (family != DeviceFamily::None && i >= 0 && i < familyProfileCount (family))
        {
            familySetProfile (s, family, i);
            eng->setSettings (s);
            applyEngineSettings();
            showMessage ("Aura Remote : profil "_t + profileName (family, i));
        }
    }
    else if (c == "playAlbum")
    {
        const auto tracks = lib.getAlbumTracks (p["artist"], p["album"]);
        if (! tracks.empty()) playTracks (tracks, 0);
    }
    else if (c == "playTrack")
    {
        if (auto t = lib.getTrack (p["id"].getLargeIntValue())) playTracks ({ *t }, 0);
    }
    if (remoteServer->isRunning()) remoteServer->setState (buildRemoteState());
}

} // namespace nx
