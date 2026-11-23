// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Brique 3 & 4: themes (CSS, hot reload, import / export), JS UI plug-ins, C DSP plug-ins, and the
// whole interface rendered off-screen (screenshots of every page).
#include "TestFramework.h"
#include "../Source/DSP/E9ds.h"
#include "../Source/Core/Diagnostics.h"
#include "FakeUpnp.h"
#include <set>
#include "TestAudioFiles.h"
#include "CoverFixtures.h"
#include "../Source/Core/ImageDecode.h"
#include "../Source/Core/LicenseManager.h"
#include "../Source/Core/LicensePublicKey.h"
#include "../Source/Core/UpdateChecker.h"
#include "LicenseFixture.h"
#include "../Source/GUI/Logo.h"
#include "NXPlayerData.h"
#include "../Source/Ext/ThemeEngine.h"
#include "../Source/Ext/PluginManager.h"
#include "../Source/GUI/AppController.h"
#include "../Source/GUI/MainComponent.h"
#include "../Source/GUI/Windows.h"
#include "../Source/Output/CaptureBackend.h"
#include "../Source/Platform/MediaControls.h"
#include "../Source/GUI/LibraryActions.h"
#include "../Source/GUI/Dashboard.h"
#include "../Source/Remote/RemoteServer.h"
#include "../Source/Library/ArtistInfo.h"
#include "../Source/DSP/StudioTools.h"
#include "../Source/UI/SplashScreen.h"

using namespace nxtest;

namespace
{
    juce::File resources()
    {
        return nx::AppController::defaultResourcesDir();   // next to the executable, or the bundle's Resources on macOS
    }

    /** How many distinct colours an image has (a blank render has very few). */
    int colourVariety (const juce::Image& img)
    {
        std::set<juce::uint32> colours;
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < bd.height; y += 3)
            for (int x = 0; x < bd.width; x += 3)
                colours.insert (bd.getPixelColour (x, y).getARGB() & 0xF0F0F0F0u);
        return (int) colours.size();
    }

    juce::Image coverArt (juce::Colour a, juce::Colour b, const juce::String& text)
    {
        juce::Image img (juce::Image::RGB, 600, 600, true);
        juce::Graphics g (img);
        g.setGradientFill (juce::ColourGradient (a, 0, 0, b, 600, 600, false));
        g.fillAll();
        for (int i = 0; i < 6; ++i)
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f + 0.04f * (float) i));
            g.fillEllipse (60.0f + i * 50.0f, 80.0f + i * 30.0f, 300.0f - i * 30.0f, 300.0f - i * 30.0f);
        }
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (64.0f, juce::Font::bold));
        g.drawText (text, img.getBounds().removeFromBottom (160), juce::Justification::centred);
        return img;
    }

    void saveJpegInto (const juce::File& f, const juce::Image& img)
    {
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::JPEGImageFormat jpg;
        jpg.setQuality (0.9f);
        jpg.writeImageToStream (img, out);
    }

    bool savePng (const juce::Image& img, const juce::File& f)
    {
        f.deleteFile();
        juce::FileOutputStream out (f);
        return juce::PNGImageFormat().writeImageToStream (img, out);
    }
}

//==============================================================================
NX_TEST_SECTION (401, "Thèmes .nxtheme : CSS, rechargement à chaud, import / export")
{
    const auto vars = nx::parseCssVariables (R"(
        /* commentaire */ :root { --a: #00F5A0; --b: var(--a); --c: rgba(10, 20, 30, 0.5); --d: var(--missing, #123); --size: 14px; }
        .x { --e: #ABC; })");
    juce::Colour c;
    t.check (vars.at ("--b") == "#00F5A0" && vars.at ("--d") == "#123" && vars.at ("--e") == "#ABC", "variables CSS, var() et valeurs de repli");
    t.check (nx::parseCssColour (vars.at ("--c"), c) && c.getRed() == 10 && std::abs (c.getFloatAlpha() - 0.5f) < 0.01f
             && nx::parseCssColour ("#ABC", c) && c == juce::Colour (0xffaabbcc), "couleurs #RGB, #RRGGBB, rgba()");

    nx::ThemeManager tm;
    auto userDir = t.tempDir.getChildFile ("themes401");
    userDir.createDirectory();
    tm.setSearchFolders ({ resources().getChildFile ("Themes"), userDir });
    t.check (tm.getThemeIds().contains ("nx-emerald") && tm.getThemeIds().contains ("aura-neon") && tm.getThemeIds().contains ("glass-rose")
             && ! tm.getThemeIds().contains ("ocean-blue") && tm.getThemeIds().contains ("pure-light") && tm.getThemeIds().contains ("solar-amber")
             && tm.getThemeIds().contains ("cobalt-minimal") && tm.getThemeIds().contains ("aura") && tm.getThemeIds().size() == 7,
             "7 thèmes livrés (Aura par défaut, NX Emerald secondaire) : "_t + tm.getThemeIds().joinIntoString (", "));
    t.check (tm.apply ("aura") && tm.current().accent() == juce::Colour (0xff2563eb) && tm.current().bg() == juce::Colour (0xff07050a)
             && tm.current().accent2() == juce::Colour (0xff29abe2), "Aura : fond #07050A, bleu du logo #2563EB, dégradé #29ABE2"_t);
    t.check (tm.apply ("nx-emerald") && tm.current().accent() == juce::Colour (0xff00f5a0) && tm.current().bg() == juce::Colour (0xff0a0c0e),
             "NX Emerald : fond #0A0C0E, accent #00F5A0");
    t.check (tm.findTheme ("aura-neon") != nullptr && tm.findTheme ("aura-neon")->shaderCode.contains ("gl_FragColor"),
             "Aura Neon : shader GLSL chargé"_t);

    // hot reload: a copy of NX Emerald in the user folder, edited while applied
    resources().getChildFile ("Themes/nx-emerald.nxtheme").copyDirectoryTo (userDir.getChildFile ("live.nxtheme"));
    auto manifest = userDir.getChildFile ("live.nxtheme/manifest.json");
    manifest.replaceWithText (manifest.loadFileAsString().replace ("\"nx-emerald\"", "\"live\""));
    tm.rescan();
    int changes = 0;
    juce::Colour seen;
    tm.onThemeChanged = [&] (const nx::Theme& th) { ++changes; seen = th.accent(); };
    tm.apply ("live");
    juce::Thread::sleep (1100);
    auto css = userDir.getChildFile ("live.nxtheme/theme.css");
    css.replaceWithText (css.loadFileAsString().replace ("--nx-accent: #00F5A0;", "--nx-accent: #FF00AA;"));
    tm.checkForChanges();
    t.check (changes == 2 && seen == juce::Colour (0xffff00aa), "rechargement à chaud : theme.css modifié -> nouvel accent appliqué"_t);

    // export -> import round trip
    auto zip = t.tempDir.getChildFile ("live.nxtheme.zip");
    t.check (tm.exportTheme ("live", zip) && zip.getSize() > 200, "export en archive .nxtheme");
    userDir.getChildFile ("live.nxtheme").deleteRecursively();
    tm.rescan();
    juce::String err;
    const auto id = tm.importTheme (zip, err);
    t.check (id == "live" && tm.findTheme ("live") != nullptr && tm.findTheme ("live")->accent() == juce::Colour (0xffff00aa),
             "import d'une archive : thème « live » restauré"_t + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
}

NX_TEST_SECTION (402, "Plug-ins d'interface JavaScript")
{
    nx::PluginManager pm;
    auto userPlugins = t.tempDir.getChildFile ("plugins402");
    userPlugins.getChildFile ("UI").createDirectory();
    pm.setSearchFolders ({ resources().getChildFile ("Plugins"), userPlugins });
    const auto plugins = pm.getUiPlugins();
    t.check (plugins.size() >= 2, juce::String ((int) plugins.size()) + " plug-ins d'interface trouvés"_t);

    pm.setUiPluginEnabled ("nx.spectrum", true);
    pm.setUiPluginEnabled ("nx.analog-vu", true);
    juce::Array<juce::var> spectrum;
    for (int i = 0; i < 64; ++i) spectrum.add (0.3 + 0.6 * std::abs (std::sin (i * 0.2)));
    auto* audio = new juce::DynamicObject();
    audio->setProperty ("spectrum", spectrum);
    audio->setProperty ("rmsL", 0.12); audio->setProperty ("rmsR", 0.2);
    audio->setProperty ("peakL", 0.5); audio->setProperty ("peakR", 0.95);
    pm.setState ("audio", juce::var (audio));
    auto* theme = new juce::DynamicObject();
    theme->setProperty ("accent", "#00F5A0"); theme->setProperty ("accent2", "#00D9F5");
    theme->setProperty ("text", "#EEF3F1"); theme->setProperty ("textDim", "#8A9792");
    pm.setState ("theme", juce::var (theme));

    for (const char* id : { "nx.spectrum", "nx.analog-vu" })
    {
        juce::Image img (juce::Image::ARGB, 420, 170, true);
        bool ok = true;
        for (int frame = 0; frame < 5; ++frame)
        {
            juce::Graphics g (img);
            g.fillAll (juce::Colour (0xff0a0c0e));
            pm.setState ("time", frame / 60.0);
            ok &= pm.render (id, "nowplaying.side", g, img.getBounds().toFloat());
        }
        savePng (img, t.tempDir.getChildFile (juce::String ("plugin_") + id + ".png"));
        t.check (ok && colourVariety (img) > 8, juce::String (id) + " : rendu JavaScript (" + juce::String (colourVariety (img)) + " teintes)");
    }

    // errors are contained, layout requests reach the host, edits are hot-reloaded
    auto dir = userPlugins.getChildFile ("UI/broken");
    dir.createDirectory();
    dir.getChildFile ("manifest.json").replaceWithText (R"({"id":"t.broken","name":"Cassé","main":"plugin.js","slots":["home.top"],"fps":10})");
    dir.getChildFile ("plugin.js").replaceWithText ("function render(g, w, h, nx) { this is not javascript }");
    pm.rescan();
    pm.setUiPluginEnabled ("t.broken", true);
    juce::Image img (juce::Image::ARGB, 300, 60, true);
    bool rendered = true;
    {
        juce::Graphics g (img);
        rendered = pm.render ("t.broken", "home.top", g, img.getBounds().toFloat());
    }
    t.check (! rendered, "script invalide : erreur affichée dans l'emplacement, l'application continue"_t);

    juce::var layoutSeen;
    pm.onLayoutRequest = [&] (const juce::var& l) { layoutSeen = l; };
    juce::Thread::sleep (1100);
    dir.getChildFile ("plugin.js").replaceWithText ("let n = 0; function render(g, w, h, nx) { n++; g.color('#FF0000'); g.rect(0, 0, w, h); if (n === 1) nx.ui.setLayout({ playerPosition: 'top' }); }");
    pm.checkForChanges();
    {
        juce::Graphics g (img);
        rendered = pm.render ("t.broken", "home.top", g, img.getBounds().toFloat());
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    t.check (rendered && img.getPixelAt (10, 10).getRed() > 200, "rechargement à chaud du script corrigé"_t + (rendered ? juce::String() : " : " + pm.getUiError ("t.broken")));
    t.check (layoutSeen.getProperty ("playerPosition", {}).toString() == "top", "nx.ui.setLayout() transmis à l'interface"_t);
}

NX_TEST_SECTION (403, "Plug-in DSP en C (ABI nxplayer_dsp.h) dans la chaîne d'écoute")
{
    nx::PluginManager pm;
    pm.setSearchFolders ({ resources().getChildFile ("Plugins") });
    const auto dsp = pm.getDspPlugins();
    const auto* cf = [&] () -> const nx::DspPluginInfo* { for (auto& p : dsp) if (p.id == "nx.crossfeed") return &p; return nullptr; }();
    t.check (cf != nullptr && cf->error.isEmpty() && cf->params.size() == 3,
             "crossfeed chargé (bibliothèque partagée, 3 paramètres)"_t + (cf != nullptr && cf->error.isNotEmpty() ? " : " + cf->error : juce::String()));
    if (cf == nullptr || cf->error.isNotEmpty()) return;

    pm.setDspPluginEnabled ("nx.crossfeed", true);
    auto chain = pm.buildDspChain();
    chain->prepare (96000.0, 2, 4096);
    std::vector<double> l (4096, 0.0), r (4096, 0.0);
    for (int i = 0; i < 4096; ++i) l[(size_t) i] = 0.5 * std::sin (2 * juce::MathConstants<double>::pi * 300.0 * i / 96000.0);
    double* ch[] = { l.data(), r.data() };
    chain->process (ch, 2, 4096);
    double energyR = 0;
    for (auto v : r) energyR += v * v;
    t.check (chain->isActive() && energyR > 1.0, "voie gauche seule -> la droite reçoit la part filtrée (crossfeed actif)"_t);

    // in the engine: the Signal Path shows the DSP and the stream is no longer bit-perfect
    const auto pcm = randomPcm (40000, 2, 24, 403);
    auto f = t.tempDir.getChildFile ("dsp403.wav");
    writeWav (f, &pcm, nullptr, 2, 96000, 24, 24, false);
    auto cap = std::make_unique<nx::CaptureBackend>();
    auto* capture = cap.get();
    nx::PlaybackEngine engine (std::move (cap));
    engine.setDspChain (pm.buildDspChain());
    nx::PlayableItem item; item.uri = f.getFullPathName(); item.title = "dsp";
    engine.setQueue ({ item }, 0, true);
    nx::SignalPath path;
    for (int i = 0; i < 4000 && engine.getState() != nx::PlaybackEngine::State::Stopped; ++i)
    {
        engine.serviceMessageThread();
        if (! capture->isRunning()) { juce::Thread::sleep (2); continue; }
        while (engine.isDecoderBusy() && engine.getBufferedBytes() < (1u << 18)) juce::Thread::sleep (1);
        capture->pump (2048);
        if (i == 5) path = engine.getSignalPath();
    }
    bool hasDsp = false;
    for (auto& n : path.nodes) hasDsp |= n.stage == "DSP" && n.title.contains ("Crossfeed");
    bool differs = false;
    for (size_t i = 1000; i < 2000 && i < capture->capturedInt.size(); ++i) differs |= capture->capturedInt[i] != pcm[i];
    t.check (hasDsp && path.overall == nx::PathQuality::Enhanced && differs, "Signal Path : « DSP · Crossfeed casque », plus bit-perfect"_t);
}

NX_TEST_SECTION (404, "Édition Free / Premium : clés de licence Ed25519, mises à jour")
{
    namespace L = nx::licensing;
    // Ed25519 itself: RFC 8032, test 1 (empty message)
    {
        L::PublicKey pub {};
        uint8_t sig[64];
        L::fromHex ("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pub.data(), 32);
        L::fromHex ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sig, 64);
        std::array<uint8_t, 32> seed {};
        L::fromHex ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed.data(), 32);
        L::SecretKey sk; L::PublicKey derived;
        L::keyPairFromSeed (seed, sk, derived);
        uint8_t mine[64];
        crypto_ed25519_sign (mine, sk.data(), nullptr, 0);
        t.check (derived == pub && crypto_ed25519_check (sig, pub.data(), nullptr, 0) == 0 && std::equal (mine, mine + 64, sig),
                 "Ed25519 (Monocypher) : vecteur de test RFC 8032 n° 1, clé publique et signature identiques"_t);
    }
    // base32
    {
        std::vector<uint8_t> back;
        const uint8_t data[] = { 'f', 'o', 'o', 'b', 'a', 'r' };
        t.check (L::base32Encode (data, 6) == "MZXW6YTBOI" && L::base32Decode ("MZXW6YTBOI", back) && back == std::vector<uint8_t> (data, data + 6)
                 && ! L::base32Decode ("MZXW6YTBO1", back), "Base32 (RFC 4648) : « foobar » -> MZXW6YTBOI, et retour"_t);
    }
    // keys
    const auto pub = licensefixture::publicKey();
    const auto key = licensefixture::key ("Élodie Martin", "elodie@example.com");
    {
        L::License l;
        const bool ok = L::verifyKey (key, pub, l) == L::Result::Ok && l.name == "Élodie Martin" && l.email == "elodie@example.com" && l.edition == "premium";
        // tampering: one character of the payload, of the signature, another key pair, junk
        auto badPayload = key; badPayload[7] = badPayload[7] == 'A' ? 'B' : 'A';
        auto badSig = key; badSig[badSig.size() - 3] = badSig[badSig.size() - 3] == 'A' ? 'B' : 'A';
        L::License x;
        const bool tamper = L::verifyKey (badPayload, pub, x) != L::Result::Ok && L::verifyKey (badSig, pub, x) == L::Result::BadSignature;
        const bool otherKey = L::verifyKey (key, L::embeddedPublicKey(), x) == L::Result::BadSignature;   // the shipped key does not accept it
        const bool junk = L::verifyKey ("NXP1-", pub, x) == L::Result::Malformed && L::verifyKey ("hello", pub, x) == L::Result::Malformed
                          && L::verifyKey (licensefixture::key ("A", "a@b.c", "enterprise"), pub, x) == L::Result::UnknownEdition;
        std::string spaced;
        for (size_t i = 0; i < key.size(); ++i) { spaced += (char) std::tolower ((unsigned char) key[i]); if (i % 20 == 19) spaced += i % 40 == 39 ? "\n" : " "; }
        const bool typed = L::verifyKey (spaced, pub, x) == L::Result::Ok;
        t.check (ok && key.rfind ("NXP1-", 0) == 0, "clé NXP1 : charge utile (nom, e-mail, édition, date) signée, vérifiée avec la clé publique seule"_t);
        t.check (tamper && otherKey && junk, "clé modifiée, signée par une autre clé, d'une autre édition ou illisible : refusée"_t);
        t.check (typed, "clé recopiée en minuscules, avec espaces et retours à la ligne : acceptée"_t);
        t.check (L::signKey ([] { L::License l; l.name = "a|b"; l.email = "x"; return l; }(), [] { L::SecretKey s; L::PublicKey p; licensefixture::pair (s, p); return s; }()).empty(),
                 "générateur : champs contenant « | » refusés"_t);
    }
    // the manager: activation, licensee, restore, Studio Master rules
    {
        nx::LicenseManager m (pub);
        juce::String err;
        const bool refused = ! m.activate ("NXP1-AAAA-BBBB", err) && ! m.isPremium() && err.isNotEmpty();
        const bool on = m.activate (juce::String::fromUTF8 (key.c_str()), err) && m.isPremium() && m.getLicensee() == juce::String::fromUTF8 ("Élodie Martin <elodie@example.com>");
        nx::LicenseManager restored (pub);
        restored.restore (m.getKey());
        nx::LicenseManager shipped;   // embedded public key
        shipped.restore (m.getKey());
        m.deactivate();
        using LM = nx::LicenseManager;
        const bool rules = ! LM::isStudioMaster (false, 192000.0, "/m/a.flac") && ! LM::isStudioMaster (false, 44100.0, "/m/a.mp3")
                           && LM::isStudioMaster (false, 352800.0, "/m/a.wav") && LM::isStudioMaster (false, 384000.0, "/m/a.flac")
                           && LM::isStudioMaster (false, 768000.0, "/m/a.wav") && LM::isStudioMaster (true, 2822400.0, "/m/a.dsf")
                           && LM::isStudioMaster (true, 22579200.0, "/m/a.dff") && LM::isStudioMaster (false, 0.0, "/m/disc.iso#area=0&track=1");
        t.check (refused && on && restored.isPremium() && ! shipped.isPremium() && ! m.isPremium(),
                 "LicenseManager : activation, titulaire, relecture au lancement, désactivation"_t);
        t.check (rules, "Free : PCM jusqu'à 192 kHz ; DXD (352,8 / 384 / 768 kHz), DSD64-512 et SACD = Studio Master"_t);
    }

    // updates: parser, semantic versions, OS download link, check through a local file
    {
        using U = nx::UpdateChecker;
        const bool sem = U::compareVersions ("1.1.0", "1.0.0") > 0 && U::compareVersions ("1.0.0", "1.0.0") == 0 && U::compareVersions ("v1.10.0", "1.9.9") > 0
                         && U::compareVersions ("1.0.0-beta.2", "1.0.0") < 0 && U::compareVersions ("1.0.0-beta.10", "1.0.0-beta.2") > 0
                         && U::compareVersions ("1.0.0-alpha", "1.0.0-beta") < 0 && U::compareVersions ("2.0", "1.99.99") > 0 && U::compareVersions ("1.0.0+build7", "1.0.0") == 0;
        t.check (sem, "versions sémantiques : 1.1.0 > 1.0.0, 1.10 > 1.9.9, 1.0.0-beta < 1.0.0, v1.10.0, métadonnées de build ignorées"_t);
        const juce::String json = R"({ "latestVersion": "1.2.0", "releaseNotes": "Nouveautes", "downloadUrlMac": "https://x/mac.dmg",
                                       "downloadUrlWin": "https://x/win.exe", "downloadUrlLinux": "https://x/linux.tar.gz", "mandatory": true })";
        nx::UpdateInfo info;
        juce::String err;
        const bool parsed = U::parse (json, info, err) && info.latestVersion == "1.2.0" && info.mandatory && info.releaseNotes == "Nouveautes"
                            && info.downloadUrlForThisOS().startsWith ("https://x/");
       #if JUCE_MAC
        const bool os = info.downloadUrlForThisOS().endsWith ("mac.dmg");
       #elif JUCE_WINDOWS
        const bool os = info.downloadUrlForThisOS().endsWith ("win.exe");
       #else
        const bool os = info.downloadUrlForThisOS().endsWith ("linux.tar.gz");
       #endif
        const bool bad = ! U::parse ("{ pas du json", info, err) && ! U::parse ("{ \"releaseNotes\": \"x\" }", info, err) && ! U::parse ("", info, err);
        t.check (parsed && os && bad, "version.json : champs lus, lien de l'OS courant, fichier invalide refusé"_t
                 + " [" + juce::String ((int) parsed) + juce::String ((int) os) + juce::String ((int) bad) + " " + err + "]");
        t.check (U::evaluate (json, "1.0.0").state == U::State::Available && U::evaluate (json, "1.2.0").state == U::State::UpToDate
                 && U::evaluate (json, "1.3.0-beta").state == U::State::UpToDate && U::evaluate ("[]", "1.0.0").state == U::State::Failed,
                 "comparaison avec la version courante : disponible / à jour / erreur"_t);
        {
            // release notes written as a list of lines, English variant; text of the "update available" window
            const juce::String j2 = R"({ "latestVersion": "1.0.1", "releaseNotes": ["- Correctif A", "- Correctif B"],
                                         "releaseNotesEn": "- Fix A", "downloadUrlMac": "m", "downloadUrlWin": "w", "downloadUrlLinux": "l" })";
            nx::UpdateInfo i2;
            const bool ok = U::parse (j2, i2, err) && i2.releaseNotes == "- Correctif A\n- Correctif B" && i2.releaseNotesEn == "- Fix A";
            const auto fr = nx::AppController::updateMessage (i2, "1.0.0", "fr"), en = nx::AppController::updateMessage (i2, "1.0.0", "en");
            t.check (ok && fr.contains ("1.0.0") && fr.contains ("1.0.1") && fr.contains ("- Correctif B") && en.contains ("- Fix A") && ! en.contains ("Correctif"),
                     "mise à jour : notes en liste de lignes (FR / EN), fenêtre avec version installée, nouvelle version et nouveautés"_t);
        }

        const auto file = t.tempDir.getChildFile ("version.json");
        file.replaceWithText (json);
        U checker ("1.0.0");
        checker.setUrl (file.getFullPathName());
        bool called = false;
        checker.checkNow ([&] (const U::Status&) { called = true; });
        for (int i = 0; i < 200 && ! called; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        const auto st = checker.getStatus();
        U missing ("1.0.0");
        missing.setUrl (t.tempDir.getChildFile ("absent.json").getFullPathName());
        bool done2 = false;
        missing.checkNow ([&] (const U::Status&) { done2 = true; });
        for (int i = 0; i < 200 && ! done2; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        t.check (called && st.state == U::State::Available && st.info.latestVersion == "1.2.0" && missing.getStatus().state == U::State::Failed,
                 "vérification en tâche de fond (fichier version.json) : mise à jour 1.2.0 trouvée ; source absente = échec propre"_t);
        const auto repoFile = juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("version.json");
        nx::UpdateInfo repo;
        t.check (juce::String (U::defaultUrl) == "https://raw.githubusercontent.com/Lennymendes/nx-audio-releases/main/version.json",
                 "mises à jour : version.json lu dans le dépôt public nx-audio-releases (fichier brut)"_t);
        t.check (! repoFile.existsAsFile() || (U::parse (repoFile.loadFileAsString(), repo, err) && U::compareVersions (repo.latestVersion, NX_VERSION) == 0),
                 "version.json du dépôt : lisible, version = celle de l'application"_t);
    }
}

NX_TEST_SECTION (301, "Interface complète hors écran (captures de chaque page)")
{
    // A small library with artwork, lyrics and several formats.
    auto music = t.tempDir.getChildFile ("music301");
    struct Alb { const char* artist; const char* album; juce::Colour a, b; int bits; double rate; bool dsd; };
    const Alb albums[] = {
        { "Quatuor Émeraude", "Nocturnes DXD", juce::Colour (0xff0b6e4f), juce::Colour (0xff08a4bd), 24, 352800, false },  // raw-utf8
        { "Zeyn", "Neon Roads", juce::Colour (0xffff2a6d), juce::Colour (0xff3a0ca3), 24, 96000, false },  // raw-utf8
        { "Ensemble Lumen", "Sacred Spaces", juce::Colour (0xffe9c46a), juce::Colour (0xff264653), 1, 5644800, true },  // raw-utf8
        { "Nora Vale", "Glass Hours", juce::Colour (0xff8ecae6), juce::Colour (0xff023047), 16, 44100, false },  // raw-utf8
        { "Atlas Trio", "Blue Hour Sessions", juce::Colour (0xff4361ee), juce::Colour (0xff111111), 24, 192000, false },  // raw-utf8
        { "Mira Solenne", "Fragments", juce::Colour (0xfff4a261), juce::Colour (0xff9d0208), 24, 88200, false },  // raw-utf8
    };
    const char* titles[] = { "Ouverture", "Lumière froide", "Night Drive", "Échos", "Sous la pluie" };  // raw-utf8
    for (auto& a : albums)
    {
        auto dir = music.getChildFile (juce::File::createLegalFileName (juce::String::fromUTF8 (a.album)));
        dir.createDirectory();
        saveJpegInto (dir.getChildFile ("cover.jpg"), coverArt (a.a, a.b, juce::String::fromUTF8 (a.album).substring (0, 1)));
        for (int k = 0; k < 4; ++k)
        {
            const auto name = juce::String (k + 1).paddedLeft ('0', 2) + " " + juce::String::fromUTF8 (titles[k]);
            if (a.dsd)
            {
                writeDsf (dir.getChildFile (name + ".dsf"), dsdSine (a.rate, 0.2, 2, 440.0 * (k + 1), 0.4), 2, a.rate, true);
            }
            else
            {
                const auto pcm = sinePcm ((int) (a.rate * 0.2), 2, a.bits, a.rate, 220.0 * (k + 1), 0.3);
                writeWav (dir.getChildFile (name + ".wav"), &pcm, nullptr, 2, a.rate, a.bits == 16 ? 16 : 24, a.bits, false);
            }
        }
    }
    music.getChildFile ("Neon Roads/03 Night Drive.lrc").replaceWithText (
        "[00:00.00]Les phares glissent sur l'asphalte\n[00:00.05]Néon bleu, néon rose\n[00:00.10]On roule jusqu'à l'aube\n[00:00.15]Night drive\n"_t
        "[00:00.18]Dernière ligne\n"_t);

    nx::AppController::Options opts;
    opts.dataDir = t.tempDir.getChildFile ("appdata301");
    opts.dataDir.createDirectory();
    opts.dataDir.getChildFile ("settings.json").replaceWithText (R"({ "theme": "ocean-blue" })");   // a 1.0 beta profile
    opts.resourcesDir = resources();
    opts.createWindow = false;
    auto cap = std::make_unique<nx::CaptureBackend>();
    cap->record = false;
    auto* virtualDac = cap.get();
    opts.backend = std::move (cap);
    opts.licensePublicKey = licensefixture::publicKey();
    nx::AppController app (std::move (opts));
    t.check (app.theme().id == "cobalt-minimal" && app.settings().get ("theme").toString() == "cobalt-minimal",
             "thème Ocean Blue d'un ancien profil : basculé sur Cobalt Minimal"_t);
    app.applyThemeNow ("aura");
    {
        // the interface tests below use every Premium feature
        juce::String licenceError;
        t.check (! app.isPremium() && app.engine().getSettings().freeEdition
                 && app.activateLicense (licensefixture::key ("Banc d'essai", "test@nxplayer.local"), licenceError) && app.isPremium()
                 && ! app.engine().getSettings().freeEdition,
                 "édition : Free au premier lancement, Premium après activation d'une clé ("_t + licenceError + ")");
    }

    // Tags for the generated WAVs come from folder / file names; set artists through the library.
    app.library().addFolder (music);
    app.scanner().scanFolderNow (music);
    for (auto& tr : app.library().getTracks ("title", 1000))
    {
        for (auto& a : albums)
            if (tr.album == juce::String::fromUTF8 (a.album) || juce::File (tr.externalId).getParentDirectory().getFileName() == juce::File::createLegalFileName (juce::String::fromUTF8 (a.album)))
            {
                auto copy = tr;
                copy.artist = copy.albumArtist = juce::String::fromUTF8 (a.artist);
                copy.album = juce::String::fromUTF8 (a.album);
                copy.title = copy.title.fromFirstOccurrenceOf (" ", false, false);
                copy.trackNumber = juce::File (tr.externalId).getFileName().substring (0, 2).getIntValue();
                copy.year = 2024;
                app.library().upsert (copy);
            }
    }
    t.check (app.library().countTracks() == 24, "bibliothèque de test : "_t + juce::String (app.library().countTracks()) + " morceaux, 6 albums");
    int withArt = 0;
    for (auto& tr : app.library().getTracks ("title", 100)) withArt += tr.artworkKey.isNotEmpty() ? 1 : 0;
    t.check (withArt == 24, "pochettes cover.jpg associées : "_t + juce::String (withArt));

    auto* main = app.getMainComponent();
    auto shots = t.tempDir.getChildFile ("screens");
    shots.createDirectory();
    auto settle = [&] (int ms)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < end)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            app.engine().serviceMessageThread();
            if (virtualDac->isRunning()) virtualDac->pump (64);   // the virtual DAC plays very slowly
        }
    };
    auto shoot = [&] (const juce::String& name)
    {
        settle (250);
        main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f);   // off-screen: the first paint requests the artwork
        settle (350);
        auto img = main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f);
        savePng (img, shots.getChildFile (name + ".png"));
        return colourVariety (img);
    };

    // shown in the (off-screen) window: it and all its parents visible (isShowing() needs a desktop peer)
    auto shownInMain = [&] (juce::Component* c)
    {
        for (; c != nullptr && c != main; c = c->getParentComponent()) if (! c->isVisible()) return false;
        return c == main;
    };
    // "+" buttons: find one by id anywhere in the window, open its popover, close it
    auto findInfo = [&] (const juce::String& id)
    {
        nx::InfoButton* found = nullptr;
        std::function<void (juce::Component*)> walk = [&] (juce::Component* c)
        {
            if (auto* b = dynamic_cast<nx::InfoButton*> (c); b != nullptr && b->id == id && shownInMain (b)) found = b;
            for (auto* k : c->getChildren()) walk (k);
        };
        walk (main);
        return found;
    };
    auto closePopover = [&]
    {
        if (auto* p = nx::InfoButton::lastPopover())
            if (auto* box = p->findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
        settle (200);
    };
    auto openInfo = [&] (const juce::String& id)
    {
        auto* b = findInfo (id);
        if (b == nullptr) return false;
        if (auto* page = b->findParentComponentOfClass<nx::ScrollPage>())       // bring the row into view first
        {
            page->setScrollY (juce::jmax (0, b->getY() - 260));
            settle (100);
        }
        b->triggerClick();
        settle (150);
        auto* p = nx::InfoButton::lastPopover();
        return p != nullptr && p->findParentComponentOfClass<juce::CallOutBox>() != nullptr && p->getHeight() > 120;
    };

    app.navigate ({ nx::PageId::Home });
    t.check (shoot ("01_accueil") > 60, "Accueil (grille, ajouts récents, spectre)"_t);
    app.navigate ({ nx::PageId::Albums });
    t.check (shoot ("02_albums") > 60, "Albums (pochettes, badges de résolution)"_t);
    app.settings().set ("albumView", "list");
    app.navigate ({ nx::PageId::Home });
    app.navigate ({ nx::PageId::Albums });
    {
        // list view: the artwork thumbnails are drawn opaque (the row fill used to leave them at a few % alpha)
        settle (300);
        auto img = main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f);
        savePng (img, shots.getChildFile ("02b_albums_liste.png"));
        int vivid = 0;
        for (int y = 0; y < img.getHeight(); y += 3)
            for (int x = 0; x < img.getWidth() / 2; x += 3)
                if (img.getPixelAt (x, y).getSaturation() > 0.45f && img.getPixelAt (x, y).getBrightness() > 0.35f) ++vivid;
        t.check (vivid > 400, "vue liste : pochettes opaques et nettes ("_t + juce::String (vivid) + " points colorés)");
    }
    app.settings().set ("albumView", "grid");
    app.navigate ({ nx::PageId::AlbumDetail, "Zeyn", "Neon Roads" });
    t.check (shoot ("03_album") > 60, "Détail d'album"_t);
    {
        // Patch 21 V2 bloc 2: the player bar grows with wide windows, nothing overflows or overlaps
        const auto w0 = main->getWidth(), h0 = main->getHeight();
        const int widths[] = { 1280, 1600, 1920, 2560, 3200 };
        int heights[5] = {};
        bool fits = true;
        juce::String why;
        for (int i = 0; i < 5; ++i)
        {
            main->setSize (widths[i], 1000);
            settle (60);
            auto& pl = main->getPlayer();
            heights[i] = pl.getHeight();
            const auto bar = pl.getLocalBounds();
            for (auto* c : std::initializer_list<juce::Component*> { &pl.lyricsButton, &pl.queueButton, &pl.studioButton, &pl.immersiveButton, &pl.deviceChip, &pl.favButton, &pl.shuffleButton, &pl.repeatButton })
                if (! bar.contains (c->getBounds())) { fits = false; why << widths[i] << ":" << c->getBounds().toString() << " "; }
            if (pl.studioButton.getX() < pl.queueButton.getRight() - 1 - 0 && pl.lyricsButton.getRight() > pl.queueButton.getX() + 1) { fits = false; why << "overlap@" << widths[i] << " "; }
            if (pl.lyricsButton.getX() < pl.deviceChip.getX() && pl.lyricsButton.getBounds().intersects (pl.deviceChip.getBounds())) { fits = false; why << "chip@" << widths[i] << " "; }
        }
        t.check (heights[0] == 84 && heights[1] == 84 && heights[2] > 84 && heights[2] < heights[3] && heights[3] == 104 && heights[4] == 104 && fits,
                 "barre de lecture : 84 px jusqu'à 1600 px, puis 104 px à 2560 px (1280 / 1600 / 1920 / 2560 / 3200 : "_t
                 + juce::String (heights[0]) + " / " + juce::String (heights[1]) + " / " + juce::String (heights[2]) + " / " + juce::String (heights[3]) + " / " + juce::String (heights[4])
                 + "), aucun contrôle ne déborde ni ne se chevauche " + why);
        main->setSize (2560, 1000);
        settle (150);
        shoot ("03e_barre_2560");
        main->setSize (1366, 800);
        settle (150);
        shoot ("03f_barre_1366");
        main->setSize (w0, h0);
        settle (100);
    }
    {
        // album cover full size: click on the cover, closes with a click, the button or Esc
        auto& listing = main->getListing();
        auto& viewer = main->getArtworkViewer();
        listing.getHeroArt().click();
        settle (150);
        const auto ib = viewer.getImageBounds();
        const bool opened = viewer.isVisible() && ib.getWidth() == ib.getHeight() && ib.getWidth() >= juce::jmin (viewer.getWidth(), viewer.getHeight()) * 6 / 10
                            && viewer.getLocalBounds().contains (ib);
        shoot ("03b_pochette_grand");
        main->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        const bool escClosed = ! viewer.isVisible();
        listing.getHeroArt().click();
        viewer.closeButton.triggerClick();
        settle (60);
        const bool buttonClosed = ! viewer.isVisible();
        listing.getHeroArt().click();
        const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> (40, 40), juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &viewer, &viewer, juce::Time::getCurrentTime(), juce::Point<float> (40, 40), juce::Time::getCurrentTime(), 1, false);
        viewer.mouseUp (click);
        const bool clickClosed = ! viewer.isVisible();
        t.check (opened && escClosed && buttonClosed && clickClosed,
                 "pochette d'album en grand : carrée, "_t + juce::String (ib.getWidth()) + " px, se ferme à Échap, au bouton et au clic ["
                 + juce::String ((int) opened) + juce::String ((int) escClosed) + juce::String ((int) buttonClosed) + juce::String ((int) clickClosed) + "]");
    }
    {
        // detailed metadata: the track and album sheets, from the album page
        const auto albumTracks = app.library().getAlbumTracks ("Zeyn", "Neon Roads");
        auto& sheet = main->getDetailsSheet();
        main->getListing().infoButton.triggerClick();
        settle (150);
        const bool albumOpen = sheet.isVisible() && sheet.getSections().size() == 2 && sheet.getSections()[1].rows.size() == albumTracks.size()
                               && sheet.getText().contains ("Neon Roads");
        shoot ("03d_informations_album");
        main->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        const bool albumEsc = ! sheet.isVisible();
        const auto track = albumTracks.size() > 2 ? albumTracks[2] : albumTracks.front();
        app.showDetails (std::vector<nx::TrackInfo> { track }, {});
        settle (150);
        auto find = [&] (const juce::String& section, const juce::String& label)
        {
            for (auto& sec : sheet.getSections())
                if (sec.title.startsWith (section))
                    for (auto& r : sec.rows) if (r.label == label) return r.value;
            return juce::String();
        };
        const bool fields = find ("Audio", "Fréquence d'échantillonnage"_t) == nx::formatRate (track.sampleRate) && find ("Audio", "Profondeur"_t) == juce::String (track.bitsPerSample) + " bits"
                            && find ("Audio", "Format"_t) == track.codec && find ("Audio", "Débit moyen"_t).endsWith ("kb/s") && find ("Fichier", "Chemin"_t) == track.externalId
                            && find ("Fichier", "Taille"_t).isNotEmpty() && find ("Morceau", "Titre"_t) == track.title;
        bool tagsRead = false;
        for (auto& sec : sheet.getSections()) if (sec.title.startsWith ("Étiquettes")) tagsRead = ! sec.rows.empty();
        const bool copy = sheet.getText().contains (track.externalId) && sheet.getText().contains ("[Audio]");
        shoot ("03c_informations_morceau");
        sheet.close();
        t.check (albumOpen && albumEsc && fields && tagsRead && copy,
                 "fiche Informations : album (résumé + "_t + juce::String ((int) albumTracks.size()) + " morceaux), morceau (format, fréquence, profondeur, débit, chemin, taille, étiquettes du fichier), texte copiable ["
                 + juce::String ((int) albumOpen) + juce::String ((int) albumEsc) + juce::String ((int) fields) + juce::String ((int) tagsRead) + juce::String ((int) copy) + "]");
        // a track that is not in a readable file (NAS / stream): the library's values and a clear note
        auto remote = track;
        remote.source = "upnp:test";
        remote.externalId = "0$42";
        remote.uri = "http://nas.local/track.flac";
        const auto secs = nx::trackDetails (remote);
        bool note = false;
        for (auto& sec : secs) if (sec.title.startsWith ("Étiquettes")) for (auto& r : sec.rows) note = note || r.value.contains ("Source réseau");
        t.check (note && secs.size() >= 4, "fiche Informations d'un morceau du NAS : valeurs de la bibliothèque et note sur les étiquettes non lisibles"_t);
    }

    const auto tracks = app.library().getAlbumTracks ("Zeyn", "Neon Roads");
    app.playTracks (tracks, 2);
    settle (600);
    app.navigate ({ nx::PageId::NowPlaying });
    settle (300);
    t.check (app.currentTrack() != nullptr && app.currentTrack()->title.contains ("Night"), "lecture lancée depuis l'interface : "_t + (app.currentTrack() ? app.currentTrack()->title : juce::String()));
    t.check (shoot ("04_en_cours_paroles") > 60, "En cours : pochette, paroles synchronisées, chemin du signal"_t);
    {
        // Now Playing: the cover opens full size, the (i) opens the track sheet
        auto& np = main->getNowPlaying();
        auto& viewer = main->getArtworkViewer();
        np.getArtwork().click();
        settle (100);
        const bool cover = viewer.isVisible() && np.getArtwork().getKey() == app.currentTrack()->artworkKey;
        viewer.closeButton.triggerClick();
        settle (60);
        np.infoButton.triggerClick();
        settle (100);
        const bool sheet = main->getDetailsSheet().isVisible() && main->getDetailsSheet().getText().contains (app.currentTrack()->title);
        main->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        t.check (cover && sheet && shownInMain (&np.infoButton) && np.infoButton.getX() > np.favButton.getX(),
                 "En cours : clic sur la pochette = grand format, « i » à côté du cœur = fiche du morceau ["_t + juce::String ((int) cover) + juce::String ((int) sheet) + "]");
    }
    {
        // Patch 21 V2: heart next to the title in Now Playing, in step with the player bar, the library and the lists
        auto& np = main->getNowPlaying();
        const auto id = app.currentTrack()->id;
        const bool before = app.currentTrack()->favourite;
        const bool shown = shownInMain (&np.favButton) && np.favButton.getX() > 0;
        np.favButton.triggerClick();
        settle (150);
        auto inFavourites = [&] { for (auto& f : app.library().getFavouriteTracks()) if (f.id == id) return true; return false; };
        const bool added = app.currentTrack()->favourite != before && np.favButton.active == app.currentTrack()->favourite
                           && main->player.favButton.active == app.currentTrack()->favourite && inFavourites() == app.currentTrack()->favourite;
        shoot ("04c_en_cours_coeur");
        app.library().setFavourite (id, before);            // as a click on a list's heart does
        settle (100);
        const bool synced = np.favButton.active == before && main->player.favButton.active == before && app.currentTrack()->favourite == before;
        t.check (shown && added && synced, "En cours : cœur à droite du titre, ajout / retrait en un clic, synchronisé avec la barre de lecture, la page Favoris et les listes"_t
                 + " [" + juce::String ((int) shown) + juce::String ((int) added) + juce::String ((int) synced) + "]");
    }
    main->player.setWaveformShown (true);
    for (int i = 0; i < 60 && app.currentAnalysis() == nullptr; ++i) settle (50);
    shoot ("04b_barre_forme_onde");
    t.check (app.currentAnalysis() != nullptr && (bool) app.settings().get ("playerWaveform", false),
             "barre de lecture : forme d'onde (analyse en arrière-plan) et choix mémorisé"_t);
    main->player.setWaveformShown (false);

    // Lire / Aléatoire = the whole library, not 12 tracks; the shuffle queue is topped up from the library
    app.playCollection ("local", true);
    settle (300);
    t.check ((int) app.engine().getQueue().size() == juce::jmin (200, app.library().countTracks()),
             "Aléatoire : toute la bibliothèque (pas 12 titres), file rechargée par passes"_t);
    // Local (sidebar) returns to the album that was open instead of the grid
    main->navigate ({ nx::PageId::AlbumDetail, "Zeyn", "Neon Roads" });
    main->navigate ({ nx::PageId::Playlists });
    main->navigate ({ nx::PageId::Library, "albums", "sidebar" });
    t.check (main->getCurrentPage().page == nx::PageId::AlbumDetail, "menu Local : reste sur l'album ouvert"_t);
    main->navigate ({ nx::PageId::Library, "albums" });

    // --- Finition UX 1/3 -------------------------------------------------------------------------------------
    {
        // 1. shuffle / repeat buttons follow the engine (the collection shuffle above lit the shuffle button)
        settle (150);
        using R = nx::PlaybackEngine::Repeat;
        t.check (app.isShuffle() && app.engine().isShuffle() && main->player.shuffleButton.active,
                 "bouton Aléatoire : synchronisé avec l'état aléatoire global (Aléatoire de la bibliothèque)"_t);
        main->player.repeatButton.onClick();
        const bool all = app.engine().getRepeat() == R::All && main->player.repeatButton.active && ! main->player.repeatButton.one;
        main->player.repeatButton.onClick();
        const bool one = app.engine().getRepeat() == R::One && main->player.repeatButton.active && main->player.repeatButton.one;
        main->player.repeatButton.onClick();
        const bool off = app.engine().getRepeat() == R::Off && ! main->player.repeatButton.active && ! main->player.repeatButton.one;
        t.check (all && one && off && (int) app.settings().get ("repeat", -1) == 0, "bouton Répéter : Désactivé -> File -> Piste -> Désactivé, affichage et moteur"_t);
        main->player.shuffleButton.onClick();
        t.check (! app.engine().isShuffle() && ! main->player.shuffleButton.active && ! (bool) app.settings().get ("shuffle", true),
                 "bouton Aléatoire : un clic coupe l'aléatoire du moteur et mémorise le choix"_t);

        // 2. Local (sidebar) inside Local goes back to the root grid
        main->navigate ({ nx::PageId::AlbumDetail, "Zeyn", "Neon Roads" });
        main->navigate ({ nx::PageId::Library, "albums", "sidebar" });
        t.check (main->getCurrentPage().page == nx::PageId::Library && main->getCurrentPage().a == "albums",
                 "menu Local : déjà dans Local sur un album, un nouveau clic ramène à la grille des albums"_t);

        // 3. scroll of the grid is remembered across an album and the Back button (many albums so that it scrolls)
        std::vector<juce::String> fakeIds;
        for (int k = 0; k < 70; ++k)
        {
            nx::TrackInfo f;
            f.source = "local";
            f.externalId = f.uri = t.tempDir.getChildFile ("fake" + juce::String (k) + ".flac").getFullPathName();
            f.title = "Piste " + juce::String (k);
            f.artist = "Artiste " + juce::String (100 - k);
            f.album = "Album " + juce::String::formatted ("%02d", k);
            f.year = 1950 + k;
            f.duration = 200; f.codec = "FLAC"; f.sampleRate = 44100; f.bitsPerSample = 16; f.channels = 2;
            app.library().upsert (f);
            fakeIds.push_back (f.externalId);
        }
        app.library().notifyChanged();
        main->navigate ({ nx::PageId::Library, "albums" });
        settle (200);
        auto& listing = main->getListing();
        listing.setScrollY (900);
        const int scrolled = listing.getScrollY();
        const auto albumsNow = listing.grid.getAlbums();
        main->navigate ({ nx::PageId::AlbumDetail, albumsNow.front().artist, albumsNow.front().title });
        t.check (listing.getScrollY() == 0, "album ouvert : la page démarre en haut"_t);
        main->goBack();
        settle (100);
        t.check (scrolled > 500 && listing.getScrollY() == scrolled, "Précédent : la grille retrouve exactement sa position de scroll ("_t + juce::String (scrolled) + ")");
        listing.setScrollY (1300);
        const int s2 = listing.getScrollY();
        main->navigate ({ nx::PageId::AlbumDetail, albumsNow.front().artist, albumsNow.front().title });
        main->navigate ({ nx::PageId::Library, "albums", "sidebar" });
        t.check (main->getCurrentPage().page == nx::PageId::Library && listing.getScrollY() == s2, "Local depuis un album : retour à la grille, scroll conservé"_t);

        // 4. size / list / sort: the controls are really on top (not under the title header) and act at once
        auto& vb = listing.viewBar;
        auto topmostIs = [&] (juce::Component& c)
        {
            auto* hit = listing.content().getComponentAt (listing.content().getLocalPoint (&c, c.getLocalBounds().getCentre()));
            return hit == &c || c.isParentOf (hit);   // (a combo box answers with its own label)
        };
        const bool onTop = vb.isVisible() && topmostIs (vb.small) && topmostIs (vb.medium) && topmostIs (vb.large) && topmostIs (vb.viewToggle) && topmostIs (vb.sort);
        t.check (onTop, "S, M, L, Liste / Grille et tri : au premier plan, les clics les atteignent"_t
                 + " [vis=" + juce::String ((int) vb.isVisible()) + " s=" + juce::String ((int) topmostIs (vb.small)) + " m=" + juce::String ((int) topmostIs (vb.medium))
                 + " l=" + juce::String ((int) topmostIs (vb.large)) + " v=" + juce::String ((int) topmostIs (vb.viewToggle)) + " sort=" + juce::String ((int) topmostIs (vb.sort)) + "]");
        vb.small.onClick();
        const float sCard = listing.grid.getCardSize();
        vb.medium.onClick();
        const float mCard = listing.grid.getCardSize();
        vb.large.onClick();
        const float lCard = listing.grid.getCardSize();
        t.check (sCard == 130.0f && mCard == 180.0f && lCard == 240.0f && app.settings().get ("albumSize", "").toString() == "L",
                 "S / M / L : vignettes de 130 / 180 / 240 px, choix mémorisé"_t);
        vb.viewToggle.onClick();
        const bool list = listing.grid.isListMode();
        vb.viewToggle.onClick();
        t.check (list && ! listing.grid.isListMode(), "bouton Liste / Grille : bascule la vue en lignes détaillées puis revient à la grille"_t);
        auto sortedBy = [&] (int id, auto less)
        {
            vb.sort.setSelectedId (id, juce::sendNotificationSync);
            const auto& v = listing.grid.getAlbums();
            return v.size() > 10 && std::is_sorted (v.begin(), v.end(), less);
        };
        const bool title = sortedBy (2, [] (const nx::AlbumInfo& a, const nx::AlbumInfo& b) { return a.title.compareNatural (b.title, false) < 0; });
        const bool artist = sortedBy (3, [] (const nx::AlbumInfo& a, const nx::AlbumInfo& b) { return a.artist.compareNatural (b.artist, false) < 0; });
        const bool year = sortedBy (4, [] (const nx::AlbumInfo& a, const nx::AlbumInfo& b) { return a.year > b.year; });
        const bool added = sortedBy (5, [] (const nx::AlbumInfo& a, const nx::AlbumInfo& b) { return a.added > b.added; });
        sortedBy (1, [] (const nx::AlbumInfo&, const nx::AlbumInfo&) { return true; });
        t.check (title && artist && year && added && vb.sort.getNumItems() == 6, "menu de tri : titre A-Z, artiste, année, date d'ajout trient la vue immédiatement"_t
                 + " [" + juce::String ((int) title) + juce::String ((int) artist) + juce::String ((int) year) + juce::String ((int) added) + " n=" + juce::String ((int) listing.grid.getAlbums().size()) + "]");

        // 5. pencil on the album page: album and each track, pre-filled, view refreshed on save
        const auto first = app.library().getAlbumTracks ("Zeyn", "Neon Roads");
        main->navigate ({ nx::PageId::AlbumDetail, "Zeyn", "Neon Roads" });
        settle (150);
        auto& ed = listing.editButton;
        t.check (ed.isVisible() && listing.content().getComponentAt (listing.content().getLocalPoint (&ed, ed.getLocalBounds().getCentre())) == &ed
                 && static_cast<bool> (listing.list.onEdit),
                 "vue album : crayon de l'album au premier plan, crayon sur chaque piste"_t);
        auto modal = [&]() -> juce::AlertWindow*
        {
            settle (100);
            return juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0
                       ? dynamic_cast<juce::AlertWindow*> (juce::ModalComponentManager::getInstance()->getModalComponent (0)) : nullptr;
        };
        ed.onClick();
        if (auto* w = modal())
        {
            const bool prefilled = w->getTextEditorContents ("album") == "Neon Roads" && w->getTextEditorContents ("artist") == "Zeyn";
            w->getTextEditor ("year")->setText ("1999");
            w->exitModalState (1);
            settle (300);
            t.check (prefilled && ! listing.heroTracks.empty() && listing.heroTracks.front().year == 1999,
                     "crayon de l'album : fenêtre pré-remplie, la vue album affiche l'année enregistrée"_t);
        }
        else t.check (false, "crayon de l'album : la fenêtre d'édition s'ouvre"_t);

        const auto row = listing.list.getTracks().at (1);
        listing.list.onEdit (row);
        if (auto* w = modal())
        {
            const bool prefilled = w->getTextEditorContents ("title") == row.title && w->getTextEditorContents ("artist") == row.artist
                                   && w->getTextEditorContents ("no").getIntValue() == row.trackNumber;
            w->getTextEditor ("title")->setText ("Titre corrige");
            w->getTextEditor ("no")->setText ("9");
            w->exitModalState (1);
            settle (300);
            const auto& shown = listing.list.getTracks();
            bool seen = false;
            for (auto& s : shown) seen = seen || (s.id == row.id && s.title == "Titre corrige" && s.trackNumber == 9);
            t.check (prefilled && seen, "crayon d'une piste : fenêtre pré-remplie, la liste affiche titre et numéro enregistrés"_t
                     + " [pre=" + juce::String ((int) prefilled) + " seen=" + juce::String ((int) seen) + " n=" + juce::String ((int) shown.size()) + "]");
            // a rescan of the file keeps what was typed
            auto again = row;
            again.title = "Titre du fichier";
            app.library().upsert (again);
            bool kept = false;
            for (auto& s : app.library().getAlbumTracks ("Zeyn", "Neon Roads")) kept = kept || (s.id == row.id && s.title == "Titre corrige");
            t.check (kept, "titre modifié à la main : conservé après un nouveau scan du fichier"_t);
        }
        else t.check (false, "crayon d'une piste : la fenêtre d'édition s'ouvre"_t);

        for (auto& id : fakeIds) app.library().remove ("local", id);
        app.library().notifyChanged();
        (void) first;
        main->navigate ({ nx::PageId::Library, "albums" });
    }

    app.applyThemeNow ("aura-neon");
    app.navigate ({ nx::PageId::Albums });
    t.check (shoot ("05_theme_aura_neon") > 40 && app.theme().id == "aura-neon", "thème Aura Neon appliqué à chaud"_t);
    {
        // the three minimal themes: listed, applied hot, readable (text contrasts with the background)
        bool ok = true;
        for (auto [id, accent] : { std::pair<const char*, juce::uint32> { "pure-light", 0xff000000 }, { "solar-amber", 0xfff59e0b }, { "cobalt-minimal", 0xff3b82f6 } })
        {
            app.applyThemeNow (id);
            app.navigate ({ nx::PageId::Albums });
            const auto& th = app.theme();
            ok = ok && th.id == id && th.accent() == juce::Colour (accent)
                 && std::abs (th.textCol().getPerceivedBrightness() - th.bg().getPerceivedBrightness()) > 0.6f
                 && shoot ("05_theme_" + juce::String (id)) > 20;
        }
        t.check (ok, "thèmes Pure Light, Solar Amber et Cobalt Minimal : appliqués à chaud, texte contrasté"_t);
        {
            // hot theme: combo boxes follow at once; Aura Remote receives the theme with its exact colours
            app.applyThemeNow ("pure-light");
            app.navigate ({ nx::PageId::Albums });
            settle (100);
            int combos = 0, wrong = 0;
            std::function<void (juce::Component&)> scan = [&] (juce::Component& c)
            {
                if (auto* cb = dynamic_cast<juce::ComboBox*> (&c); cb != nullptr && cb->isVisible())
                    for (auto* ch : cb->getChildren())
                        if (auto* l = dynamic_cast<juce::Label*> (ch)) { ++combos; if (l->findColour (juce::Label::textColourId) != app.theme().textCol()) ++wrong; }
                for (auto* ch : c.getChildren()) scan (*ch);
            };
            scan (*main);
            const auto st = juce::JSON::parse (app.buildRemoteState());
            const auto th = st.getProperty ("theme", {});
            const bool remote = th.getProperty ("id", {}).toString() == "pure-light" && th.getProperty ("bg", {}).toString() == "#F4F5F7"
                                && th.getProperty ("text", {}).toString() == "#111827" && th.getProperty ("accent", {}).toString() == "#000000"
                                && (bool) th.getProperty ("isLight", false) && th.getProperty ("surfaceLight", {}).toString().length() == 9
                                && th.getProperty ("textMuted", {}).toString().startsWith ("#") && th.getProperty ("border", {}).toString().startsWith ("#")
                                && th.getProperty ("surface", {}).toString() == "#FFFFFF";
            app.applyThemeNow ("solar-amber");
            const auto th2 = juce::JSON::parse (app.buildRemoteState()).getProperty ("theme", {});
            const bool follows = th2.getProperty ("accent", {}).toString() == "#F59E0B" && ! (bool) th2.getProperty ("isLight", true);
            t.check (combos > 0 && wrong == 0 && remote && follows,
                     "thème à chaud (listes déroulantes comprises) et thème transmis à Aura Remote (couleurs hexadécimales, clair / sombre)"_t
                     + " [" + juce::String (combos) + "/" + juce::String (wrong) + juce::String ((int) remote) + juce::String ((int) follows) + "]");
        }
    }
    app.applyThemeNow ("aura");

    app.navigate ({ nx::PageId::Search, "lum" });
    t.check (shoot ("06_recherche") > 40, "Recherche unifiée"_t);
    // Streaming sources are off: the sidebar keeps local files and the NAS only.
    t.check (app.providers().getProvider ("qobuz") == nullptr && app.providers().getProvider ("spotify") == nullptr
             && app.providers().getProvider ("upnp") != nullptr, "sources : fichiers locaux et NAS uniquement (streaming retiré)"_t);

    // NAS: a fake UPnP server on 127.0.0.1 that serves the Neon Roads files.
    auto nasMedia = music.getChildFile ("Neon Roads");
    FakeUpnpServer nas (nasMedia, "Synology DS920+");
    {
        using N = FakeUpnpServer::Node;
        N music0; music0.id = "music"; music0.parent = "0"; music0.title = "Musique"; music0.container = true; nas.add (music0);
        const char* names[] = { "Neon Roads", "Glass Hours", "Fragments", "Blue Hour Sessions" };
        for (int i = 0; i < 4; ++i)
        {
            N a; a.id = "a" + juce::String (i); a.parent = "music"; a.title = names[i]; a.container = true; a.art = "cover.jpg";
            a.artist = i == 0 ? "Zeyn" : "Artiste " + juce::String (i);
            nas.add (a);
        }
        int k = 0;
        for (const auto& e : juce::RangedDirectoryIterator (nasMedia, false, "*.wav"))
        {
            N it; it.id = "n" + juce::String (++k); it.parent = "a0"; it.title = e.getFile().getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false);
            it.artist = "Zeyn"; it.album = "Neon Roads"; it.file = e.getFile().getFileName(); it.rate = 96000; it.bits = 24; it.art = "cover.jpg";
            nas.add (it);
        }
    }
    auto* upnp = dynamic_cast<nx::UpnpProvider*> (app.providers().getProvider ("upnp"));
    if (upnp != nullptr) upnp->discoverHost ("127.0.0.1", nas.udpPort, 3000);
    const auto nasServers = upnp != nullptr ? upnp->getServers() : std::vector<nx::UpnpServer>();
    t.check (nasServers.size() == 1, "NAS trouvé par l'interface : "_t + (nasServers.empty() ? juce::String ("aucun") : nasServers[0].name));
    app.navigate ({ nx::PageId::Source, "upnp" });
    t.check (shoot ("07_nas_serveurs") > 30, "NAS : serveurs du réseau"_t);
    if (! nasServers.empty())
    {
        app.navigate ({ nx::PageId::Source, "upnp", nx::SourcePage::upnpArg (nasServers[0].udn, "music", { nasServers[0].name, "Musique" }) });
        settle (600);
        t.check (shoot ("08_nas_dossier") > 40, "NAS : dossier d'albums (grille avec pochettes)"_t);
        app.navigate ({ nx::PageId::Source, "upnp", nx::SourcePage::upnpArg (nasServers[0].udn, "a0", { nasServers[0].name, "Musique", "Neon Roads" }) });
        settle (600);
        shoot ("08b_nas_album");
    }

    app.navigate ({ nx::PageId::Settings });
    t.check (shoot ("09_reglages") > 30, "Réglages (sortie, DSD, paroles, téléchargements)"_t);
    {
        bool all = true;
        juce::String missing;
        for (auto* id : { "rate", "fades", "pcmdsd", "keepdsd" }) if (findInfo (id) == nullptr) { all = false; missing << id << " "; }
        const bool opened = openInfo ("pcmdsd");
        shoot ("09b_reglages_explication");
        closePopover();
        t.check (all && opened && nx::InfoButton::lastPopover() == nullptr,
                 "Réglages › Audio : « + » sur Fréquence de sortie, Micro-fondus, PCM -> DSD / XZ Automatic, Garder la sortie DSD ; fenêtre ouverte puis fermée"_t
                 + (missing.isNotEmpty() ? " [" + missing + "]" : juce::String()));
        // the texts: three points each, all translated to English
        const auto english = juce::JSON::parse (juce::String::fromUTF8 (NXData::en_json, NXData::en_jsonSize));
        bool complete = true, translated = true;
        juce::String untranslated;
        for (auto& id : nx::explainerIds())
        {
            const auto e = nx::explainer (id);
            complete = complete && e.title.isNotEmpty() && e.what.length() > 40 && e.gain.length() > 30 && e.limits.length() > 30;
            for (auto& str : { e.what, e.gain, e.limits })
                if (! english.hasProperty (juce::Identifier (str))) { translated = false; untranslated << id << " "; }
        }
        for (auto* head : { "CE QUE ÇA FAIT", "GAIN RÉEL À L'ÉCOUTE", "COMPROMIS / LIMITES" })
            if (! english.hasProperty (juce::Identifier (juce::String::fromUTF8 (head)))) { translated = false; untranslated << head << " "; }
        t.check (complete && translated, "explications « + » : 10 fiches (ce que ça fait, gain à l'écoute, compromis), toutes traduites en anglais"_t
                 + (untranslated.isNotEmpty() ? " [" + untranslated + "]" : juce::String()));
    }
    app.plugins().setUiPluginEnabled ("nx.analog-vu", true);
    app.navigate ({ nx::PageId::NowPlaying });
    t.check (shoot ("10_vu_metres") > 60, "plug-in VU-mètres dans « En cours »"_t);

    // Extensions gallery (plug-ins only: themes live in Réglages › Interface), then the user plug-ins catalogue
    auto& market = main->getMarketplace();
    app.navigate ({ nx::PageId::Marketplace });
    const int galleryCards = market.getNumCards();
    bool noThemeCard = true;
    for (int i = 0; i < galleryCards; ++i) noThemeCard = noThemeCard && market.getCard (i)->getData().kind != nx::MarketCard::Kind::Theme;
    t.check (galleryCards >= 1 && noThemeCard, "Extensions : "_t + juce::String (galleryCards) + " cartes, plug-ins d'interface et DSP seulement (aucun thème)"_t);
    t.check (shoot ("12_extensions") > 60, "galerie avec aperçus en direct"_t);
    const int themeCount = (int) app.themes().getThemes().size();
    t.check (themeCount == 7 && app.themes().findTheme ("aura") != nullptr && app.themes().findTheme ("nx-emerald") != nullptr,
             "thèmes livrés : "_t + juce::String (themeCount) + " (Aura par défaut, NX Emerald en thème secondaire)"_t);
    {
        // community themes: the 6 .json files of CommunityThemes/ (not in the binary) imported as a user would
        const auto dir = juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("CommunityThemes");
        const char* files[] = { "vice_sunset", "cyber_2077_yellow", "cyber_neon_orchid", "spartan_warthog", "halo_ring_horizon", "blacklist_baron_flame" };
        const juce::uint32 accents[] = { 0xfff36a78, 0xffdce026, 0xffc252e1, 0xff94ab57, 0xff92caeb, 0xffffd000 };
        const juce::uint32 backgrounds[] = { 0xff1a0933, 0xff121512, 0xff161230, 0xff232426, 0xff152036, 0xff0a0a0b };
        bool imported = true;
        juce::String why;
        for (int i = 0; i < 6; ++i)
        {
            juce::String err;
            const auto id = app.importThemeFile (dir.getChildFile (juce::String (files[i]) + ".json"), err);
            const auto remote = juce::JSON::parse (app.buildRemoteState()).getProperty ("theme", {});
            const bool ok = id == juce::String (files[i]).replaceCharacter ('_', '-') && app.theme().id == id
                            && app.theme().accent() == juce::Colour (accents[i]) && app.theme().bg() == juce::Colour (backgrounds[i])
                            && remote.getProperty ("id", {}).toString() == id
                            && app.themes().findTheme (id) != nullptr && app.themes().findTheme (id)->folder.isAChildOf (app.userDataDirectory());
            if (! ok) why << files[i] << " " << err << "; ";
            imported = imported && ok;
            if (i == 0) { settle (100); shoot ("50_theme_vice_sunset"); }
            if (i == 5) { settle (100); shoot ("51_theme_baron_flame"); }
        }
        const bool listed = (int) app.themes().getThemes().size() == themeCount + 6;
        // a shared file can never replace a built-in theme; unsafe content is refused
        const auto fake = t.tempDir.getChildFile ("fake_emerald.json");
        fake.replaceWithText (R"({ "nxtheme": 1, "id": "nx-emerald", "name": "Faux", "variables": { "--nx-bg": "#FF0000", "--nx-accent": "#00FF00" } })");
        juce::String err;
        const auto fakeId = app.importThemeFile (fake, err);
        const bool builtInSafe = fakeId == "nx-emerald-perso" && app.themes().findTheme ("nx-emerald")->accent() == juce::Colour (0xff00f5a0);
        const auto bad = t.tempDir.getChildFile ("bad.json");
        bad.replaceWithText (R"J({ "name": "Bad", "variables": { "--nx-bg": "#000000", "--nx-accent": "#FFFFFF", "color": "red } body { x: url(http://x)" } })J");
        const bool refused = app.importThemeFile (bad, err).isEmpty() && err.isNotEmpty();
        // export -> import round trip (with the accent override)
        app.applyThemeNow ("cobalt-minimal");
        app.setAccent (juce::Colour (0xfff59e0b));
        const auto out = t.tempDir.getChildFile ("mon_theme.json");
        const bool exported = app.exportCurrentTheme (out);
        app.setAccent (juce::Colours::transparentBlack);
        const auto back = juce::JSON::parse (out);
        nx::Theme rt;
        juce::String rtErr;
        const bool roundTrip = exported && nx::ThemeManager::themeFromJson (back, rt, rtErr) && rt.accent() == juce::Colour (0xfff59e0b)
                               && rt.bg() == app.themes().findTheme ("cobalt-minimal")->bg() && back.getProperty ("nxtheme", 0).equals (1);
        // clean up: the following tests count the shipped themes
        for (auto* f : files) app.themes().findTheme (juce::String (f).replaceCharacter ('_', '-'))->folder.deleteRecursively();
        app.themes().findTheme ("nx-emerald-perso")->folder.deleteRecursively();
        app.themes().rescan();
        app.applyThemeNow ("aura");
        t.check (imported && listed && builtInSafe && refused && roundTrip,
                 "thèmes communautaires : 6 fichiers .json importés et appliqués (Aura Remote suit), thème intégré jamais remplacé, contenu dangereux refusé, export / réimport"_t
                 + " [" + why + juce::String ((int) listed) + juce::String ((int) builtInSafe) + juce::String ((int) refused) + juce::String ((int) roundTrip) + "]");
    }
    app.navigate ({ nx::PageId::UserPlugins, "catalogue" });
    const int catalogueCards = market.getNumCards();
    t.check (catalogueCards == 5, "Plugins utilisateur › Catalogue : "_t + juce::String (catalogueCards) + " plug-ins en plus (4 interface, 1 DSP)");
    t.check (shoot ("13_plugins_catalogue") > 60, "catalogue avec aperçus"_t);

    juce::String installError;
    const bool radialOk = market.installFromCatalogue ("nx.waterfall", installError);
    bool radialOn = false;
    for (auto& p : app.plugins().getUiPlugins()) if (p.id == "nx.waterfall") radialOn = p.enabled && p.folder.isAChildOf (app.userDataDirectory());
    t.check (radialOk && radialOn, "installation en un clic : Spectrogramme copié dans le dossier utilisateur et activé"_t
                                   + (installError.isNotEmpty() ? " (" + installError + ")" : juce::String()));
    const bool toneOk = market.installFromCatalogue ("nx.tone", installError);
    bool toneOn = false;
    for (auto& p : app.plugins().getDspPlugins()) if (p.id == "nx.tone") toneOn = p.enabled && p.error.isEmpty();
    t.check (toneOk && toneOn, "plug-in DSP du catalogue installé, bibliothèque chargée et activé"_t
                               + (installError.isNotEmpty() ? " (" + installError + ")" : juce::String()));
    app.navigate ({ nx::PageId::UserPlugins, "installes" });
    t.check (market.getNumCards() == 2, "onglet Installés : "_t + juce::String (market.getNumCards()) + " plug-ins");
    app.navigate ({ nx::PageId::NowPlaying });
    shoot ("10b_spectrogramme");
    app.navigate ({ nx::PageId::UserPlugins, "installes" });
    t.check (shoot ("14_plugins_installes") > 40, "plug-ins installés par l'utilisateur"_t);
    juce::String removeError;
    const bool removed = market.removeUserPlugin ("nx.tone", removeError);
    bool stillThere = false;
    for (auto& p : app.plugins().getDspPlugins()) if (p.id == "nx.tone") stillThere = true;
    t.check (removed && ! stillThere, "retrait d'un plug-in utilisateur"_t + (removeError.isNotEmpty() ? " (" + removeError + ")" : juce::String()));
    app.plugins().setUiPluginEnabled ("nx.waterfall", false);

    // History (‹ ›) and the sidebar toggle
    app.navigate ({ nx::PageId::Home });
    app.navigate ({ nx::PageId::Albums });
    app.navigate ({ nx::PageId::Favourites });
    main->goBack();
    const bool backOk = main->getCurrentPage().page == nx::PageId::Albums;
    main->goBack();
    const bool back2Ok = main->getCurrentPage().page == nx::PageId::Home && main->canGoForward();
    main->goForward();
    const bool fwdOk = main->getCurrentPage().page == nx::PageId::Albums;
    app.navigate ({ nx::PageId::Artists });
    t.check (backOk && back2Ok && fwdOk && ! main->canGoForward(), "historique : ‹ revient en arrière, › avance, une nouvelle page efface l'avant"_t);
    {
        // shortcuts: Cmd/Ctrl + [ / ], Alt + arrows, Backspace (no text field focused), side mouse buttons 4 / 5, Cmd/Ctrl + B
        using K = juce::KeyPress;
        const auto cmd = juce::ModifierKeys::commandModifier, alt = juce::ModifierKeys::altModifier;
        app.navigate ({ nx::PageId::Albums });
        app.navigate ({ nx::PageId::Favourites });
        main->grabKeyboardFocus();
        main->keyPressed (K ('[', cmd, 0));
        const bool k1 = main->getCurrentPage().page == nx::PageId::Albums;
        main->keyPressed (K (']', cmd, 0));
        const bool k2 = main->getCurrentPage().page == nx::PageId::Favourites;
        main->keyPressed (K (K::leftKey, alt, 0));
        main->keyPressed (K (K::rightKey, alt, 0));
        const bool k3 = main->getCurrentPage().page == nx::PageId::Favourites;
        main->keyPressed (K (K::backspaceKey));
        const bool k4 = main->getCurrentPage().page == nx::PageId::Albums;
        auto mouse = [&] (juce::ModifierKeys::Flags button)
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const juce::MouseEvent e (src, { 10.0f, 10.0f }, juce::ModifierKeys (button), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      main, main, juce::Time::getCurrentTime(), { 10.0f, 10.0f }, juce::Time::getCurrentTime(), 1, false);
            main->sideButtons.mouseDown (e);
        };
        mouse (juce::ModifierKeys::forwardButtonModifier);
        const bool m1 = main->getCurrentPage().page == nx::PageId::Favourites;
        mouse (juce::ModifierKeys::backButtonModifier);
        const bool m2 = main->getCurrentPage().page == nx::PageId::Albums;
        main->keyPressed (K ('b', cmd, 0));
        const bool hidden = ! main->isSidebarVisible();
        main->keyPressed (K ('b', cmd, 0));
        t.check (k1 && k2 && k3 && k4 && m1 && m2 && hidden && main->isSidebarVisible() && ! main->backButton.isEnabled() == ! main->canGoBack(),
                 "navigation : Cmd/Ctrl + [ ], Alt + flèches, Retour arrière, boutons souris 4 / 5, Cmd/Ctrl + B (barre latérale)"_t);
        app.navigate ({ nx::PageId::Artists });
    }
    main->setSidebarVisible (false, false);
    t.check (! main->sidebar.isVisible() && main->sidebarButton.getX() <= 16, "barre latérale masquée : le contenu prend toute la largeur"_t);
    shoot ("15_barre_laterale_masquee");
    main->setSidebarVisible (true, false);

    {
        // AURA DSP (studio button): headphone profile list (search), parametric EQ (knobs, handles, presets), header chip
        auto& dsp = main->auraDsp;
        main->player.studioButton.onClick();
        const bool open = dsp.isVisible() && dsp.getView() == nx::AuraDspPanel::View::Headphone;
        const int all = dsp.visiblePresetCount();
        dsp.searchFor ("hd 650");
        const bool search = dsp.visiblePresetCount() == 1 && dsp.visiblePresetName (0) == "Sennheiser HD 650";
        dsp.searchFor ("momentum");
        const bool search2 = dsp.visiblePresetCount() == 2;
        dsp.searchFor ({});
        dsp.choosePreset (1);
        const bool clOn = app.studioTools().eqActive() && main->audioStatus.eqLabel() == "MOMENTUM M2 CL"
                          && app.settings().get ("headphoneEq").getProperty ("preset", {}).toString() == "momentum-m2-cl";
        settle (100);
        shoot ("46_aura_dsp_casque");
        dsp.chooseMonitor (1);
        const bool mono = app.studioTools().monitor.load() == nx::StudioToolsState::Monitor::Mono;
        dsp.chooseMonitor (0);
        {
            // Auto-Headroom True Peak: its row (switch, ceiling, "+"), saved, and the measurement in the Informations sheet
            const bool offFirst = ! app.engine().getSettings().autoTruePeak && ! dsp.truePeak.getToggleState();
            dsp.truePeak.triggerClick();
            settle (60);
            const bool on = app.engine().getSettings().autoTruePeak && app.engine().getSettings().truePeakCeilingDb == -1.0 && dsp.tpCeiling.getButtonText() == "-1,0 dBTP";
            dsp.tpCeiling.triggerClick();
            settle (60);
            const bool half = app.engine().getSettings().truePeakCeilingDb == -0.5 && dsp.tpCeiling.getButtonText() == "-0,5 dBTP"
                              && (double) app.settings().get ("engine").getProperty ("truePeakCeiling", 0.0) == -0.5;
            dsp.tpCeiling.triggerClick();
            settle (60);
            bool plus = false;
            for (auto* c : dsp.getChildren()) if (auto* ib = dynamic_cast<nx::InfoButton*> (c)) plus = plus || ib->id == "truepeak";
            shoot ("46b_aura_dsp_true_peak");
            const auto albumTracks = app.library().getAlbumTracks ("Zeyn", "Neon Roads");
            const auto played = albumTracks.size() > 2 ? albumTracks[2] : albumTracks.front();
            const auto stored = app.library().getTruePeak (played.externalId);
            app.showDetails (std::vector<nx::TrackInfo> { played }, {});
            settle (100);
            juce::String tpRow, ahRow;
            for (auto& sec : main->getDetailsSheet().getSections())
                for (auto& r : sec.rows) { if (r.label == "True Peak") tpRow = r.value; if (r.label == "Auto-Headroom") ahRow = r.value; }
            shoot ("46d_informations_true_peak");
            main->getDetailsSheet().close();
            dsp.truePeak.triggerClick();
            settle (60);
            t.check (offFirst && on && half && plus && stored.has_value() && tpRow.endsWith ("dBTP (ITU-R BS.1770-4)") && ahRow.contains ("dB")
                         && ! app.engine().getSettings().autoTruePeak,
                     "AURA DSP : Auto-Headroom True Peak (désactivé par défaut, plafond -1,0 / -0,5 dBTP enregistré, « + »), mesure enregistrée en base, fiche Informations : "_t
                         + tpRow + " / " + ahRow + " [" + juce::String ((int) offFirst) + juce::String ((int) on) + juce::String ((int) half) + juce::String ((int) plus)
                         + juce::String ((int) stored.has_value()) + "]");
        }
        {
            // Sous-étape E: "+ EQ paramétrique" next to the headphone list adds an EQ of its own, the profile untouched
            auto gains = [&] { juce::String g; for (auto& band : app.studioTools().parametricBands()) g << band.gain << "/" << band.freq << ";"; return g; };
            const bool shown = dsp.addPeqButton.isVisible() && dsp.addPeqButton.getButtonText() == "+ EQ paramétrique"_t && ! app.studioTools().peqActive();
            const auto before = gains();
            dsp.addPeqButton.triggerClick();
            settle (60);
            const bool added = app.studioTools().peqActive() && dsp.getView() == nx::AuraDspPanel::View::Parametric && app.studioTools().eqActive()
                               && main->audioStatus.eqLabel() == "MOMENTUM M2 CL" && gains() == before;
            dsp.getBand (2).gain.setValue (4.0, juce::sendNotificationSync);
            const auto mine = gains();
            dsp.setView (nx::AuraDspPanel::View::Headphone);
            dsp.choosePreset (0);                                          // another headphone: the parametric bands stay
            const bool independent = gains() == mine && main->audioStatus.eqLabel() == "MOMENTUM M2" && app.studioTools().peqActive()
                                     && dsp.addPeqButton.isVisible() && dsp.addPeqButton.getButtonText() == "EQ paramétrique ›"_t;
            shoot ("46c_aura_dsp_casque_plus_parametrique");
            dsp.getBand (2).gain.setValue (0.0, juce::sendNotificationSync);
            dsp.choosePreset (1);
            dsp.peq.triggerClick();                                        // off again for the steps below
            settle (60);
            dsp.setView (nx::AuraDspPanel::View::Headphone);
            t.check (shown && added && independent && ! app.studioTools().peqActive(),
                     "AURA DSP : « + EQ paramétrique » avec un profil casque, bandes propres (pas une copie), inchangées quand on change de casque"_t
                     + " [" + juce::String ((int) shown) + juce::String ((int) added) + juce::String ((int) independent) + "]");
        }
        // parametric view: knob, handle on the curve, reset, copy, user preset
        dsp.peq.triggerClick();
        settle (60);
        const bool peqView = dsp.getView() == nx::AuraDspPanel::View::Parametric && app.studioTools().peqActive() && dsp.getBand (3).isVisible();
        dsp.getBand (3).gain.setValue (5.5, juce::sendNotificationSync);
        const bool knob = std::abs (app.studioTools().parametricBands()[3].gain - 5.5) < 1e-9;
        dsp.getGraph().moveHandle (5, 8000.0, -3.2, true);
        const auto b5 = app.studioTools().parametricBands()[5];
        const bool handle = std::abs (b5.freq - 8000.0) < 0.5 && std::abs (b5.gain + 3.2) < 1e-9
                            && dsp.getGraph().handleAt (dsp.getGraph().handlePosition (5)) == 5;
        dsp.getBand (1).type.setSelectedId (2, juce::sendNotificationSync);
        const bool typeChange = app.studioTools().parametricBands()[1].type == nx::hpeq::BandType::LowShelf;
        const auto saved = app.settings().get ("parametricEq");
        const bool persisted = (bool) saved.getProperty ("enabled", false) && saved.getProperty ("bands", {}).size() == 7
                               && std::abs ((double) saved.getProperty ("bands", {})[5].getProperty ("freq", 0.0) - 8000.0) < 0.5;
        app.saveParametricPreset ("Mon Momentum");
        settle (100);
        shoot ("46b_aura_dsp_parametrique");
        dsp.resetButton.triggerClick();
        settle (60);
        bool flat = true;
        for (auto& band : app.studioTools().parametricBands()) flat = flat && band.gain == 0.0;
        dsp.copyButton.triggerClick();
        settle (60);
        const bool copied = app.studioTools().parametricBands()[0].gain == 4.0 && app.studioTools().parametricBands()[6].gain == 1.0;   // M2 CL treble
        const bool loaded = app.loadParametricPreset ("Mon Momentum") && std::abs (app.studioTools().parametricBands()[3].gain - 5.5) < 1e-9
                            && app.getParametricPresetNames().contains ("Mon Momentum");
        // both equalisers together: headroom on the combined peak
        const double combined = nx::hpeq::peakBoostOf ([&] (double f) { return app.studioTools().combinedDb (48000.0, f); });
        const bool headroom = app.studioTools().eqActive() && app.studioTools().peqActive()
                              && std::abs (app.studioTools().eqPreampDb.load() + combined + 0.1) < 1e-6;
        dsp.close();
        main->resized();
        settle (300);
        auto click = [&] (juce::Component& c, juce::Point<int> pos)
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const juce::MouseEvent e (src, pos.toFloat(), juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                      juce::Time::getCurrentTime(), pos.toFloat(), juce::Time::getCurrentTime(), 1, false);
            c.mouseUp (e);
        };
        const bool chipShown = main->audioStatus.isVisible() && ! main->audioStatus.eqArea().isEmpty() && ! main->audioStatus.isBitPerfect();
        shoot ("47_entete_audio");
        click (main->audioStatus, main->audioStatus.eqArea().getCentre());
        const bool bypassed = ! app.studioTools().eqActive() && app.studioTools().eqPresetInfo() != nullptr
                              && (bool) app.settings().get ("headphoneEq").getProperty ("bypass", false);
        click (main->audioStatus, main->audioStatus.eqArea().getCentre());
        const bool backOn = app.studioTools().eqActive();
        click (main->audioStatus, { 10, main->audioStatus.getHeight() / 2 });
        const bool fromChip = dsp.isVisible();
        dsp.close();
        app.setHeadphoneEq (-1, false);
        app.setParametricEq (nx::hpeq::flatParametric(), false, true);
        t.check (open && all == 11 && search && search2 && clOn && mono, "AURA DSP : liste des profils casque (11), recherche « hd 650 » / « momentum », profil M2 CL, monitoring"_t
                 + " [" + juce::String (all) + juce::String ((int) open) + juce::String ((int) search) + juce::String ((int) search2) + juce::String ((int) clOn) + juce::String ((int) mono) + " " + main->audioStatus.eqLabel() + " " + juce::String ((int) app.studioTools().eqActive()) + " " + app.settings().get ("headphoneEq").getProperty ("preset", {}).toString() + "]");
        t.check (peqView && knob && handle && typeChange && persisted && flat && copied && loaded && headroom,
                 "égaliseur paramétrique 7 bandes : potentiomètre, poignée sur la courbe, type de filtre, sauvegarde, réinitialiser, copier le casque, preset utilisateur, headroom combiné"_t
                 + " [" + juce::String ((int) peqView) + juce::String ((int) knob) + juce::String ((int) handle) + juce::String ((int) typeChange) + juce::String ((int) persisted)
                 + juce::String ((int) flat) + juce::String ((int) copied) + juce::String ((int) loaded) + juce::String ((int) headroom) + "]");
        t.check (chipShown && bypassed && backOn && fromChip && ! app.studioTools().anyActive(),
                 "en-tête : témoin Bit-Perfect, EQ casque contourné / réactivé d'un clic, ouvre AURA DSP"_t
                 + " [" + juce::String ((int) chipShown) + juce::String ((int) bypassed) + juce::String ((int) backOn) + juce::String ((int) fromChip)
                 + juce::String ((int) ! app.studioTools().anyActive()) + "]");
        {
            // E9DS: row of AURA DSP, its own view (focus presets, amount, mode), remembered; DSD converted while it is on
            main->player.studioButton.onClick();
            dsp.e9ds.triggerClick();
            settle (60);
            dsp.e9Sib.triggerClick();
            dsp.e9Hf.triggerClick();
            settle (60);
            dsp.e9Amount.setValue (-4.5, juce::sendNotificationSync);
            dsp.e9Bril.triggerClick();
            settle (60);
            const auto& e = app.e9ds();
            const auto saved = app.settings().get ("e9ds");
            const bool on = dsp.getView() == nx::AuraDspPanel::View::E9ds && e.enabled.load() && dsp.e9Focus.isVisible() && ! dsp.getGraph().isVisible()
                            && e.mode.load() == nx::E9dsState::DynamicHf && std::abs (e.amountDb.load() - 4.5) < 1e-9 && e.focusHz.load() == 9000.0
                            && (bool) saved.getProperty ("enabled", false) && (double) saved.getProperty ("focus", 0.0) == 9000.0
                            && app.engine().getSettings().dspNeedsPcm;
            settle (100);
            shoot ("46d_aura_dsp_e9ds");
            bool infos = true;
            for (auto* id : { "headroom", "dxd", "crossfeed", "hpeq", "e9ds" }) infos = infos && dsp.infoFor (id) != nullptr && shownInMain (dsp.infoFor (id));
            const bool e9Info = openInfo ("e9ds");
            shoot ("46e_aura_dsp_explication_e9ds");
            closePopover();
            t.check (infos && e9Info, "AURA DSP : « + » sur Headroom, Conversion DXD, Crossfeed, Égaliseur casque, E9DS ; fenêtre E9DS en 3 points"_t);
            dsp.e9Split.triggerClick();
            dsp.e9ds.triggerClick();
            settle (60);
            const bool off = ! e.enabled.load() && ! app.engine().getSettings().dspNeedsPcm && ! (bool) app.settings().get ("e9ds").getProperty ("enabled", true);
            dsp.close();
            t.check (on && off, "AURA DSP · E9DS : vue dédiée (Brillance 9 kHz, -4,5 dB, Dynamic HF), réglages gardés, DSD converti tant qu'il est actif, éteint sans reconstruire la chaîne"_t
                     + " [" + juce::String ((int) on) + juce::String ((int) off) + "]");
        }
        {
            // no third-party product name anywhere in the interface (French and English dictionaries, sources);
            // the names are assembled here so that this file stays free of them too
            const juce::StringArray banned { juce::String ("mu") + "se", juce::String ("ci") + "der", juce::String ("ro") + "on" };
            auto hasBanned = [&] (const juce::String& lower) { for (auto& w : banned) if (lower.contains (w)) return true; return false; };
            const auto english = juce::String::fromUTF8 (NXData::en_json, NXData::en_jsonSize).toLowerCase();
            bool clean = ! hasBanned (english);
            juce::String offenders;
            const auto root = juce::File (__FILE__).getParentDirectory().getParentDirectory();
            for (auto* dir : { "Source", "Tests" })
                for (const auto& e : juce::RangedDirectoryIterator (root.getChildFile (dir), true, "*.cpp;*.h;*.mm"))
                    if (hasBanned (e.getFile().loadFileAsString().toLowerCase())) { clean = false; offenders << e.getFile().getFileName() << " "; }
            t.check (clean, "marque : aucun nom de logiciel tiers dans l'interface FR / EN, les sources et les tests"_t + (offenders.isNotEmpty() ? " [" + offenders + "]" : juce::String()));
        }
    }
    {
        // appearance: dark / light / system, accent colour, listening-bar density
        const auto darkBefore = app.theme().id;
        app.setAppearance ("light");
        const bool light = app.theme().id == "pure-light" && app.getAppearance() == "light";
        app.setAppearance ("dark");
        const bool dark = app.theme().id == darkBefore;
        app.setAccent (juce::Colour (0xff8b5cf6));
        const bool violet = app.theme().accent() == juce::Colour (0xff8b5cf6);
        app.setAppearance ("light");
        const bool readable = app.theme().accent().getPerceivedBrightness() <= 0.42f;
        settle (100);
        shoot ("48_apparence_clair_violet");
        app.setAppearance ("dark");
        app.setAccent (juce::Colours::transparentBlack);
        const bool signature = app.theme().accent() == juce::Colour (0xff2563eb) && app.settings().get ("accent").toString().isEmpty();
        t.check (light && dark && violet && readable && signature,
                 "apparence : Sombre / Clair, couleur d'accent (assombrie sur thème clair), retour au bleu Aura #2563EB"_t);
        app.setLayoutOption ("density", "integrated");
        const bool flush = main->player.getX() == 0 && main->player.getRight() == main->getWidth() && main->player.getBottom() == main->getHeight();
        settle (100);
        shoot ("49_barre_compacte_integree");
        app.setLayoutOption ("density", "compact");
        const bool compactH = main->player.getHeight() == main->playerHeight() && main->playerHeight() < 84 && main->player.getX() > 0;
        app.setLayoutOption ("density", "comfortable");
        t.check (flush && compactH && main->player.getHeight() == 84, "densité de la barre d'écoute : Confortable, Compact, Compact intégré (bord de fenêtre)"_t);
        // official links: Discord hidden while the invitation is unknown; the shop and Discord can be set in settings.json
        const bool hidden = app.discordUrl().isEmpty();
        app.settings().set ("discordUrl", "https://discord.gg/nxaudio-test");
        app.settings().set ("purchaseUrl", "javascript:alert(1)");
        const bool links = app.discordUrl() == "https://discord.gg/nxaudio-test" && app.shopUrl() == juce::String (nx::links::shop);
        auto& sw3 = main->getSettingsWindow();
        app.navigate ({ nx::PageId::Settings, "apropos" });
        settle (100);
        bool discordButton = false;
        for (auto* c : sw3.getPage().content().getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Rejoindre la communauté NX Audio"_t) discordButton = true;
        app.navigate ({ nx::PageId::Settings, "audio" });     // the next tests open the settings on their last tab
        sw3.close();
        app.settings().set ("discordUrl", juce::var());
        app.settings().set ("purchaseUrl", juce::var());
        t.check (hidden && links && discordButton, "liens officiels : Discord masqué sans invitation, affiché une fois renseigné ; boutique (adresse https uniquement)"_t);
    }

    // Device profiles: the Merging Anubis banner (forced, the virtual DAC has another name), then RME
    {
        auto st = app.engine().getSettings();
        st.hardwareProfile = nx::HardwareProfile::MergingRavenna;
        app.engine().setSettings (st);
        app.navigate ({ nx::PageId::Settings });
        t.check (shoot ("16_profil_anubis") > 40, "Réglages : profils Merging Anubis (3 profils, astuce MT VAD)"_t);
        {   // the equipment manager: compact (search + a ~190 px list that scrolls), fully visible, page ends with a margin
            auto& page = main->getSettingsWindow().getPage();
            juce::Component* catalog = nullptr;
            juce::Viewport* list = nullptr;
            for (auto* c : page.content().getChildren())
                for (auto* k : c->getChildren())
                    if (auto* v = dynamic_cast<juce::Viewport*> (k); v != nullptr && v->getHeight() == 190) { catalog = c; list = v; }
            t.check (catalog != nullptr && catalog->getHeight() < 310, "Réglages : gestionnaire d'équipements compact ("_t + juce::String (catalog != nullptr ? catalog->getHeight() : 0) + " px)");
            if (catalog != nullptr)
            {
                const int rowsH = list->getViewedComponent()->getHeight();
                list->setViewPosition (0, 1000);
                t.check (rowsH >= 5 * 38 && list->getViewPositionY() == rowsH - 190 && catalog->getCachedComponentImage() != nullptr,
                         "gestionnaire : 5 équipements en lignes fines, liste défilante (swipe), cadre mis en cache image"_t);
                list->setViewPosition (0, 0);
                page.setScrollY (catalog->getY() - 16);
                settle (100);
                t.check (catalog->getBottom() <= page.getScrollY() + page.getHeight(), "Réglages : le gestionnaire d'équipements tient dans la zone visible, sans troncature"_t);
                shoot ("16c_gestionnaire_equipements");
            }
            page.setScrollY (100000);
            settle (100);
            int lowest = 0;
            for (auto* c : page.content().getChildren()) if (c != catalog) lowest = juce::jmax (lowest, c->getBottom());
            t.check (page.getScrollY() + page.getHeight() >= page.content().getHeight(), "Réglages : le bas de la page est atteignable"_t);
            page.setScrollY (0);
        }
        st.hardwareProfile = nx::HardwareProfile::RmeAdi2;
        app.engine().setSettings (st);
        app.navigate ({ nx::PageId::Settings });
        t.check (shoot ("17_profil_rme") > 40, "Réglages : profils RME ADI-2 (sécurité DSD Direct)"_t);
        st.hardwareProfile = nx::HardwareProfile::Auto;
        app.engine().setSettings (st);
    }
    // Step 4: equipment catalogue (search, apply, save) and the Streamer / Gaming mode
    {
        t.check (nx::deviceModels().size() == 5 && nx::searchDeviceModels ("adi").size() == 1 && nx::searchDeviceModels ("pro ae").size() == 1
                 && nx::searchDeviceModels ("ka17").size() == 1 && nx::searchDeviceModels ("anubis pro").size() == 1 && nx::searchDeviceModels ("zzz").empty()
                 && juce::String (nx::deviceModels().back().id) == "generic",
                 "catalogue : RME ADI-2 Series (une seule fiche), Anubis Premium, Anubis Pro / SPS, KA17, générique en dernier"_t);
        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
        t.check (nx::findDeviceModel ("rme-adi2")->badges() == "DSD" + dot + "DoP" + dot + "DXD" && nx::findDeviceModel ("merging-anubis-pro")->badges() == "PCM 192 kHz"
                 && nx::findDeviceModel ("merging-anubis-premium")->badges() == "DSD" + dot + "DoP" + dot + "DXD" && nx::findDeviceModel ("fiio-ka17")->badges() == "DSD" + dot + "DoP" + dot + "DXD"
                 && juce::String::fromUTF8 (nx::findDeviceModel ("rme-adi2")->subtitle) == "DAC FS" + dot + "Pro FS" + dot + "Pro AE",
                 "badges : ADI-2 Series DSD · DoP · DXD (DAC FS · Pro FS · Pro AE), Anubis Pro / SPS « PCM 192 kHz »"_t);
        t.check (nx::findDeviceModel ("rme-adi2-dac") == nx::findDeviceModel ("rme-adi2") && nx::findDeviceModel ("rme-adi2-pro-ae") == nx::findDeviceModel ("rme-adi2")
                 && nx::canonicalModelId ("rme-adi2-pro") == "rme-adi2", "anciens identifiants RME (DAC, Pro, Pro AE) repris par ADI-2 Series"_t);
        auto st = app.engine().getSettings();
        nx::applyDeviceModel (st, *nx::findDeviceModel ("merging-anubis-pro"));
        app.engine().setSettings (st);
        app.applyEngineSettings();
        const auto pro = nx::resolveProfile (app.engine().getSettings(), "Carte quelconque");
        t.check (pro.dsdMode == nx::DsdMode::ConvertToPcm && ! pro.dopAllowed && pro.profileTag == "Anubis Ref" && pro.maxDsdToPcmRate == 176400.0 && ! pro.upsampleToDxd,
                 "Anubis Pro / SPS : ni DSD ni DoP ni DXD, tout reste à 192 kHz au plus (DSD converti en 176,4 kHz)"_t);
        nx::applyDeviceModel (st, *nx::findDeviceModel ("merging-anubis-premium"));
        st.anubisProfile = nx::AnubisProfile::PureDoP;
        t.check (nx::resolveProfile (st, "Carte quelconque").dsdMode == nx::DsdMode::DoP, "Anubis Premium : DoP disponible"_t);
        nx::applyDeviceModel (st, *nx::findDeviceModel ("fiio-ka17"));
        t.check (nx::resolveProfile (st, "USB DAC").dsdMode == nx::DsdMode::DoP, "FiiO KA17 : DoP, profil appliqué à un pilote non reconnu"_t);
        st.fiioProfile = 1;
        t.check (nx::resolveProfile (st, "USB DAC").dsdMode == nx::DsdMode::ConvertToPcm, "FiiO KA17 : profil DXD"_t);
        nx::applyDeviceModel (st, *nx::findDeviceModel ("rme-adi2-pro-ae"));   // an id saved by an older version
        app.engine().setSettings (st);
        app.applyEngineSettings();
        t.check (app.settings().get ("engine").getProperty ("deviceModel", {}).toString() == "rme-adi2"
                 && st.hardwareProfile == nx::HardwareProfile::RmeAdi2, "équipement choisi enregistré dans les préférences (ADI-2 Series)"_t);
        st.hardwareProfile = nx::HardwareProfile::Auto;
        st.deviceModel = {};
        st.anubisProfile = nx::AnubisProfile::StudioDxdReference;   // do not leak Pure DoP / FiiO choices into the next tests
        st.fiioProfile = 0;
        app.engine().setSettings (st);
        app.applyEngineSettings();

        bool uiBefore = false;
        for (auto& p : app.plugins().getUiPlugins()) uiBefore = uiBefore || (p.id == "nx.spectrum" && p.enabled);
        app.setStreamerMode (true);
        {
            // visual extensions off for the session, greyed in Extensions; the transport is never throttled
            bool anyUiOn = false;
            for (auto& p : app.plugins().getUiPlugins()) anyUiOn = anyUiOn || p.enabled;
            app.navigate ({ nx::PageId::Marketplace, "interface" });
            settle (100);
            int greyed = 0, uiCards = 0;
            for (auto* c : main->getMarketplace().content().getChildren())
                if (auto* card = dynamic_cast<nx::MarketCard*> (c); card != nullptr && card->getData().kind == nx::MarketCard::Kind::UiPlugin)
                {
                    ++uiCards;
                    if (card->getData().suspended && ! card->isToggleEnabled() && card->getAlpha() < 0.9f) ++greyed;
                }
            shoot ("16e_extensions_mode_streamer");
            nx::perf::setBackground (true);
            app.tickForTests();
            const bool clockFluid = main->player.getClockIntervalMs() <= 34 && app.getTickIntervalMs() <= 67;
            nx::perf::setBackground (false);
            // OBS export: queued in a few microseconds, written by the worker thread
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            app.exportNowPlaying();
            const double queued = juce::Time::getMillisecondCounterHiRes() - t0;
            t.check (uiBefore && ! anyUiOn && uiCards > 0 && greyed == uiCards && clockFluid && queued < 5.0,
                     "mode Streamer : extensions visuelles coupées et grisées, horloge et contrôles jamais bridés, export OBS hors du fil principal"_t
                     + " [" + juce::String (greyed) + "/" + juce::String (uiCards) + ", " + juce::String (queued, 2) + " ms]");
        }
        nx::perf::setBackground (false);
        t.check (app.isStreamerMode() && (bool) app.settings().get ("streamerMode", false) && main->streamerButton.active
                 && nx::perf::hz (30, 4) == 4, "mode Streamer : activé, enregistré, bouton allumé, timers ralentis"_t);
        nx::perf::setBackground (true);
        const bool backgroundSlow = nx::perf::hz (30, 4) == 1;
        nx::perf::setBackground (false);
        main->player.stateChanged();
        settle (200);
        shoot ("16b_mode_streamer");
        {
            // OBS: now_playing.txt / now_playing_cover.jpg, written by the controller while Streamer mode is on
            const auto txt = app.userDataDirectory().getChildFile ("now_playing.txt");
            for (int i = 0; i < 40 && ! txt.existsAsFile(); ++i) settle (50);
            const auto* tr = app.currentTrack();
            const bool exported = txt.existsAsFile() && (tr == nullptr || txt.loadFileAsString().contains (tr->title));
            const auto dir = t.tempDir.getChildFile ("obs");
            dir.createDirectory();
            nx::TrackInfo fake;
            fake.title = "Night Drive";
            fake.artist = "Zeyn";
            juce::Image img (juce::Image::RGB, 64, 64, true);
            juce::Graphics (img).fillAll (juce::Colours::orange);
            const bool written = nx::AppController::writeNowPlayingFiles (dir, &fake, img)
                                 && dir.getChildFile ("now_playing.txt").loadFileAsString() == juce::String::fromUTF8 ("Zeyn \xe2\x80\x94 Night Drive")
                                 && juce::ImageFileFormat::loadFrom (dir.getChildFile ("now_playing_cover.jpg")).getWidth() == 64;
            const bool cleared = nx::AppController::writeNowPlayingFiles (dir, nullptr, {})
                                 && dir.getChildFile ("now_playing.txt").loadFileAsString().isEmpty() && ! dir.getChildFile ("now_playing_cover.jpg").exists();
            app.setNowPlayingOverlay (true);
            settle (150);
            auto* ov = app.getOverlayWindow();
            const bool overlay = app.isNowPlayingOverlayVisible() && ov != nullptr && ov->getWidth() == 460;
            if (ov != nullptr)
                savePng (ov->getOverlay().createComponentSnapshot (ov->getOverlay().getLocalBounds(), true, 1.0f), shots.getChildFile ("16d_overlay_now_playing.png"));
            app.setNowPlayingOverlay (false);
            t.check (backgroundSlow && exported && written && cleared && overlay && ! app.isNowPlayingOverlayVisible()
                     && ! (bool) app.settings().get ("nowPlayingOverlay", true),
                     "mode Streamer : 1 image / s en arrière-plan, now_playing.txt + pochette JPEG pour OBS, overlay « Now Playing »"_t
                     + " [" + juce::String ((int) backgroundSlow) + juce::String ((int) exported) + juce::String ((int) written) + juce::String ((int) cleared) + juce::String ((int) overlay) + "]");
        }
        app.setStreamerMode (false);
        nx::perf::setBackground (false);
        {
            bool restored = false;
            for (auto& p : app.plugins().getUiPlugins()) restored = restored || (p.id == "nx.spectrum" && p.enabled);
            t.check (restored && app.settings().get ("streamerSuspendedUi").isVoid(), "mode Streamer coupé : extensions visuelles restaurées"_t);
        }
        t.check (! app.isStreamerMode() && nx::perf::hz (30, 4) == 30, "mode Streamer : désactivé, cadence normale"_t);
    }
    // A-B loop markers on the seek bar
    {
        const double len = app.engine().getDurationSeconds();
        if (len > 0.1) app.engine().setLoop (len * 0.3, len * 0.7);
        main->player.trackChanged();
        app.navigate ({ nx::PageId::NowPlaying });
        shoot ("18_boucle_ab");
        t.check (app.engine().isLoopActive(), "boucle A-B posée depuis l'interface"_t);
        main->player.clearLoop();
    }

    //==========================================================================================
    // v1.0 interface: sidebar pillars, quality pills, favourites / playlists (drag & drop), artist pages,
    // NAS badges and clean-up, settings window + dashboard, profile chip, studio tools, immersive view,
    // onboarding, Aura Remote, English.
    app.artistInfo().setEnabled (false);      // no network in the tests: artist data are stored by hand below
    auto& sb = main->sidebar;
    sb.refresh();
    const int iLocal = sb.findItem (nx::PageId::Library, "albums"), iFav = sb.findItem (nx::PageId::Favourites);
    const int iPl = sb.findItem (nx::PageId::Playlists), iNas = sb.findItem (nx::PageId::Source, "upnp"), iExt = sb.findItem (nx::PageId::Marketplace);
    t.check (iLocal >= 0 && iFav > iLocal && iPl > iFav && iNas > iPl && iExt > iNas && sb.findItem (nx::PageId::UserPlugins) < 0,
             "barre latérale épurée : Local, Favoris, Playlists, NAS / Jellyfin, Extensions"_t);
    {
        // rebranding: the player is "Aura" everywhere the user can read (English strings and the phone page included)
        const auto en = juce::String::fromUTF8 (NXData::en_json, NXData::en_jsonSize);
        const auto web = juce::String::fromUTF8 (NXData::index_html, NXData::index_htmlSize);
        const juce::String oldName = juce::String ("NX") + " Player";
        t.check (! en.contains (oldName) && ! web.contains (oldName) && ! en.contains ("NX Remote") && juce::JUCEApplication::getInstance()->getApplicationName() == "Aura"
                 && nx::ThemeManager().current().accent() == juce::Colour (0xff2563eb),
                 "nouveau nom « Aura » partout (application, anglais, Aura Remote) ; bleu du logo par défaut"_t);
    }
    {
        // bug reports: one zip with the summary and the logs, nothing private
        nx::audioLog ("test: ligne de journal");
        const auto zipFile = nx::diagnostics::exportZip (app.diagnosticsSummary(), t.tempDir.getChildFile ("logs"));
        juce::ZipFile zip (zipFile);
        juce::String summary;
        if (auto* e = zip.getEntry ("diagnostic.txt"))
            if (std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (*e)); in != nullptr) summary = in->readEntireStreamAsString();
        const auto key = app.settings().get ("licenseKey").toString();
        t.check (zipFile.existsAsFile() && zip.getEntry ("audio.log") != nullptr && summary.startsWith ("Aura ") && summary.contains ("Sortie")
                 && (key.isEmpty() || ! summary.contains (key)) && zip.getEntry ("settings.json") == nullptr,
                 "Réglages › À propos › Exporter les journaux : zip (résumé système, audio.log, rapports de plantage), sans clé ni réglages"_t);
    }

    auto& listing = main->getListing();
    auto albumTitles = [&] { juce::StringArray a; for (auto& x : listing.grid.getAlbums()) a.add (x.title); a.sort (true); return a.joinIntoString (", "); };
    app.navigate ({ nx::PageId::Library, "dxd" });
    t.check (listing.pills.getNumPills() == 9 && listing.pills.getActive() == "dxd" && albumTitles() == "Nocturnes DXD",
             "pastille DXD : "_t + albumTitles());
    t.check (shoot ("19_filtres_qualite") > 40, "barre de filtres par qualité (Albums | Artistes | Pistes | Genres | Ajouts récents | DXD | DSD | Hi-Res | CD)"_t);
    app.navigate ({ nx::PageId::Library, "dsd" });
    const auto dsdAlbums = albumTitles();
    app.navigate ({ nx::PageId::Library, "hires" });
    const auto hiresAlbums = albumTitles();
    app.navigate ({ nx::PageId::Library, "cd" });
    const auto cdAlbums = albumTitles();
    t.check (dsdAlbums == "Sacred Spaces" && hiresAlbums == "Blue Hour Sessions, Fragments, Neon Roads, Nocturnes DXD" && cdAlbums == "Glass Hours",
             "DSD : "_t + dsdAlbums + " · Hi-Res : " + hiresAlbums + " · CD : " + cdAlbums);
    app.navigate ({ nx::PageId::Albums });
    t.check (listing.pills.isVisible() && listing.pills.getActive() == "albums", "les anciennes vues (Albums...) gardent les pastilles"_t);

    // favourites and playlists by drag & drop on the sidebar
    const auto neon = app.library().getAlbumTracks ("Zeyn", "Neon Roads");
    const auto glass = app.library().getAlbumTracks ("Nora Vale", "Glass Hours");
    t.check (sb.dropTracks ({ neon[0].id, neon[1].id }, iFav) == "favourites" && app.library().getFavouriteTracks().size() == 2,
             "glisser deux morceaux sur Favoris"_t);
    app.library().setAlbumFavourite ("Atlas Trio", "Blue Hour Sessions", true);
    const auto plId = nx::actions::createPlaylistWith (app, "Soirée DXD"_t, { neon[2].id });
    sb.refresh();
    const int iMine = sb.findItem (nx::PageId::PlaylistDetail, juce::String (plId));
    sb.dropTracks (nx::actions::trackIds (glass), iMine);
    t.check (iMine >= 0 && app.library().getPlaylistTracks (plId).size() == 1 + glass.size(),
             "glisser un album entier sur une playlist : "_t + juce::String ((int) app.library().getPlaylistTracks (plId).size()) + " morceaux");
    app.navigate ({ nx::PageId::Favourites });
    t.check (listing.grid.getAlbums().size() == 1 && listing.list.getTracks().size() == 2, "page Favoris : 1 album, 2 morceaux"_t);
    t.check (shoot ("20_favoris") > 40, "page Favoris"_t);

    // artist page (data as the TheAudioDB service would store them)
    {
        nx::ArtistMeta m;
        m.name = "Zeyn";
        m.found = true;
        m.imageUrl = neon.front().artworkKey;
        m.bannerUrl = app.library().getAlbumTracks ("Mira Solenne", "Fragments").front().artworkKey;
        m.bio = "Zeyn est un producteur et ingénieur du son français, figure de la scène électronique « néon ». "
                "Ses albums, enregistrés en DXD 352,8 kHz puis mastérisés en DSD, explorent les nuits urbaines et les longues routes.\n\n"
                "Neon Roads, sorti en 2024, a été mixé sur AuraDAW et converti avec AuraEngine. "
                "Sur scène, il joue sur une Merging Anubis reliée en RAVENNA à un système de diffusion multicanal."_t;
        m.bioSource = "TheAudioDB";
        m.genre = "Electronic";
        m.country = "Paris, France";
        m.fetched = juce::Time::currentTimeMillis();
        app.library().setArtistMeta (m);
    }
    app.navigate ({ nx::PageId::ArtistDetail, "Zeyn" });
    t.check (listing.artistHeader.isVisible() && listing.artistHeader.hasBio() && listing.artistHeader.hasPhoto() && ! listing.grid.getAlbums().empty(),
             "page artiste : bannière, photo, biographie et discographie de la bibliothèque"_t);
    t.check (shoot ("21_page_artiste") > 60, "page artiste"_t);
    app.navigate ({ nx::PageId::Library, "artistes" });
    t.check (listing.grid.roundArt && listing.grid.getAlbums().size() == 6, "vue Artistes en médaillons ronds"_t);
    shoot ("22_artistes");

    // NAS: import the server's album, badge, clean-up
    if (upnp != nullptr && ! nasServers.empty())
    {
        const int imported = upnp->importContainer (app.library(), nasServers[0], "a0");
        app.navigate ({ nx::PageId::Albums });
        nx::AlbumInfo neonAlbum;
        for (auto& a : listing.grid.getAlbums()) if (a.title == "Neon Roads") neonAlbum = a;
        t.check (imported == 4 && neonAlbum.fromNas() && app.networkSourceLabel (neonAlbum.nasSource) == "NAS",
                 "album importé du NAS : badge « NAS » à côté du format"_t);
        t.check (shoot ("23_badges_nas") > 40, "badges NAS sur les pochettes"_t);
        const int removed = nx::actions::removeNasAlbums (app, { neonAlbum });
        bool stillLocal = false;
        for (auto& a : app.library().getAlbums ("artist", 100)) if (a.title == "Neon Roads") stillLocal = ! a.fromNas();
        t.check (removed == 4 && stillLocal, "« Retirer de la bibliothèque » : les 4 morceaux du NAS partent, l'album local reste"_t);
        upnp->importContainer (app.library(), nasServers[0], "a0");
        const int purged = nx::actions::purgeNas (app);
        t.check (purged == 4 && app.library().countAlbumsFromSourcePrefix ("upnp") == 0, "« Purger les éléments du NAS » depuis les réglages"_t);
    }

    // Settings window (modal) with the hardware dashboard; the virtual DAC is renamed as a Merging Anubis
    const auto originalDac = virtualDac->name;
    virtualDac->name = "MT VAD (Merging Anubis Premium)";
    app.selectDevice (virtualDac->name);
    app.navigate ({ nx::PageId::Settings, "audio" });
    auto& sw = main->getSettingsWindow();
    t.check (sw.isOpen() && sw.getTab() == "audio" && main->getCurrentPage().page != nx::PageId::Settings,
             "Réglages : fenêtre modale par-dessus la page (roue crantée), onglet Audio & Système"_t);
    t.check (shoot ("24_tableau_de_bord") > 60, "tableau de bord ORDINATEUR ↔ PÉRIPHÉRIQUE AUDIO (Anubis)"_t);
    {
        // smooth scrolling: no system / driver call while painting, static cards cached as images
        nx::HardwareDashboard* dash = nullptr;
        for (auto* c : sw.getPage().content().getChildren()) if (auto* d = dynamic_cast<nx::HardwareDashboard*> (c)) dash = d;
        bool cached = dash != nullptr && dash->getCachedComponentImage() != nullptr;
        const int reads = dash != nullptr ? dash->getRefreshCount() : -1;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int y = 0; y < 2000; y += 100)
        {
            sw.getPage().setScrollY (y);
            main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f);
        }
        const double perFrame = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
        sw.getPage().setScrollY (0);
        t.check (cached && dash->getRefreshCount() == reads,
                 "Audio & Système : 20 images de défilement sans aucune lecture du pilote ni du système, tableau de bord en cache image"_t);
        juce::ignoreUnused (perFrame);
    }
    {
        nx::DeviceCapabilities caps;
        caps.sampleRates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        using RS = nx::HardwareDashboard::RateSupport;
        const bool matrix = nx::HardwareDashboard::supportFor (caps, 176400.0, false) == RS::Pcm && nx::HardwareDashboard::supportFor (caps, 384000.0, false) == RS::None
                         && nx::HardwareDashboard::supportFor (caps, 2822400.0, true) == RS::DoP && nx::HardwareDashboard::supportFor (caps, 5644800.0, true) == RS::None;
        caps.nativeDsdRates = { 11289600.0 };
        t.check (matrix && nx::HardwareDashboard::supportFor (caps, 11289600.0, true) == RS::Native,
                 "matrice des fréquences : PCM, DSD en DoP (176,4 kHz -> DSD64) et DSD natif"_t);
    }
    sw.getPage().setTab ("bibliotheque");
    shoot ("25_reglages_bibliotheque");
    sw.getPage().setTab ("interface");
    t.check (shoot ("26_reglages_interface") > 30, "onglet Interface : langue, thème, disposition, vue immersive, relancer l'assistant"_t);
    {
        // Partie 3/3 — reassignable keyboard shortcuts
        namespace sc = nx::shortcuts;
        using K = juce::KeyPress;
        const auto cmd = juce::ModifierKeys::commandModifier;
        sc::resetAll (app.settings());
        t.check (sc::get (app.settings(), sc::Action::PlayPause) == K (K::spaceKey) && sc::get (app.settings(), sc::Action::Next) == K (K::rightKey, cmd, 0)
                 && sc::get (app.settings(), sc::Action::VolumeUp) == K (K::upKey, cmd, 0) && sc::get (app.settings(), sc::Action::Mute) == K ('m')
                 && sc::get (app.settings(), sc::Action::Streamer) == K ('s', cmd | juce::ModifierKeys::shiftModifier, 0)
                 && sc::get (app.settings(), sc::Action::Search) == K ('f', cmd, 0) && sc::get (app.settings(), sc::Action::PageBack) == K ('[', cmd, 0)
                 && sc::get (app.settings(), sc::Action::PageForward) == K (']', cmd, 0) && sc::all().size() == 12,
                 "raccourcis : 12 actions, touches par défaut (Espace, Cmd/Ctrl + flèches, M, S, R, Cmd/Ctrl + Maj + S, Cmd/Ctrl + F, Cmd/Ctrl + [ / ])"_t);

        // they act through the main window
        auto& eng = app.engine();
        const bool wasShuffle = app.isShuffle();
        main->keyPressed (K ('s'));
        const bool shuffleOk = app.isShuffle() != wasShuffle && main->player.shuffleButton.active == app.isShuffle();
        main->keyPressed (K ('s'));
        main->keyPressed (K ('r'));
        const bool repeatOk = eng.getRepeat() == nx::PlaybackEngine::Repeat::All;
        main->keyPressed (K ('r')); main->keyPressed (K ('r'));
        main->player.stepVolume (-0.5);
        const double v0 = main->player.getVolumePosition();
        main->keyPressed (K (K::upKey, cmd, 0));
        const double v1 = main->player.getVolumePosition();
        main->keyPressed (K (K::downKey, cmd, 0));
        main->keyPressed (K ('m'));
        const double muted = main->player.getVolumePosition();
        main->keyPressed (K ('m'));
        const double restored = main->player.getVolumePosition();
        main->keyPressed (K ('s', cmd | juce::ModifierKeys::shiftModifier, 0));
        const bool streamer = app.isStreamerMode();
        main->keyPressed (K ('s', cmd | juce::ModifierKeys::shiftModifier, 0));
        t.check (shuffleOk && repeatOk && eng.getRepeat() == nx::PlaybackEngine::Repeat::Off && v1 > v0 && muted == 0.0 && std::abs (restored - v0) < 1e-9
                 && streamer && ! app.isStreamerMode(),
                 "raccourcis actifs : aléatoire, répéter, volume +/−, muet puis retour au niveau, mode Streamer"_t);
        main->player.stepVolume (1.0);
        settle (50);   // the settings page is rebuilt after a theme / extensions change (asynchronously)

        // reassign from the settings: click a row, press the new keys; a key already used is refused; reset
        juce::Component* muteRow = nullptr;
        for (auto* c : sw.getPage().content().getChildren()) if (c->getTitle() == nx::tr ("Muet")) muteRow = c;
        bool reassigned = false, refused = false;
        if (muteRow != nullptr)
        {
            muteRow->postCommandMessage (1);
            settle (50);
            muteRow->keyPressed (K ('k', cmd | juce::ModifierKeys::altModifier, 0));
            reassigned = sc::get (app.settings(), sc::Action::Mute) == K ('k', cmd | juce::ModifierKeys::altModifier, 0)
                         && sc::match (app.settings(), K ('m')) == std::nullopt
                         && app.settings().get ("shortcuts").getProperty ("mute", {}).toString().isNotEmpty();
            muteRow->postCommandMessage (1);
            settle (50);
            muteRow->keyPressed (K (K::spaceKey));                       // already "Lecture / Pause"
            refused = sc::get (app.settings(), sc::Action::Mute) == K ('k', cmd | juce::ModifierKeys::altModifier, 0);
        }
        main->keyPressed (K ('k', cmd | juce::ModifierKeys::altModifier, 0));
        const bool newKeyWorks = main->player.getVolumePosition() == 0.0;
        main->keyPressed (K ('k', cmd | juce::ModifierKeys::altModifier, 0));
        sw.getPage().setTab ("interface");
        shoot ("26b_raccourcis_clavier");
        sc::resetAll (app.settings());
        t.check (muteRow != nullptr && reassigned && refused && newKeyWorks && sc::get (app.settings(), sc::Action::Mute) == K ('m'),
                 "Réglages › Raccourcis clavier : réassignation par clic + touches, doublon refusé, enregistré, réinitialisation"_t);
        {
            // every row of the list: a real mouse click, then F1..F12 (+ Cmd + Alt): stored, matched, old key free
            sw.getPage().setTab ("interface");
            settle (50);
            int rows = 0, ok = 0;
            juce::StringArray failed;
            for (auto& info : sc::all())
            {
                juce::Component* row = nullptr;
                for (auto* c : sw.getPage().content().getChildren()) if (c->getTitle() == nx::tr (juce::String::fromUTF8 (info.label))) row = c;
                if (row == nullptr) { failed.add (juce::String::fromUTF8 (info.id) + "?"); continue; }
                ++rows;
                const auto oldKey = sc::get (app.settings(), info.action);
                const K newKey (K::F1Key + (int) info.action, cmd | juce::ModifierKeys::altModifier, 0);
                const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), { 5.0f, 5.0f }, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              row, row, juce::Time::getCurrentTime(), { 5.0f, 5.0f }, juce::Time::getCurrentTime(), 1, false);
                row->mouseUp (click);
                row->keyPressed (newKey);
                if (sc::get (app.settings(), info.action) == newKey && sc::match (app.settings(), newKey) == info.action
                    && sc::match (app.settings(), oldKey) == std::nullopt)
                    ++ok;
                else failed.add (juce::String::fromUTF8 (info.id));
            }
            // the reassigned page keys drive the history, and the ‹ › buttons name them
            app.navigate ({ nx::PageId::Albums });
            app.navigate ({ nx::PageId::Favourites });
            main->keyPressed (K (K::F1Key + (int) sc::Action::PageBack, cmd | juce::ModifierKeys::altModifier, 0));
            const bool back = main->getCurrentPage().page == nx::PageId::Albums;
            main->keyPressed (K (K::F1Key + (int) sc::Action::PageForward, cmd | juce::ModifierKeys::altModifier, 0));
            const bool forward = main->getCurrentPage().page == nx::PageId::Favourites;
            main->updateNavButtons();
            const bool tip = main->getBackButton().getTooltip().contains ("F11");
            sc::resetAll (app.settings());
            main->updateNavButtons();
            app.navigate ({ nx::PageId::Settings, "interface" });
            t.check (rows == (int) sc::all().size() && ok == rows && back && forward && tip && main->getBackButton().getTooltip().contains ("["),
                     "les "_t + juce::String (rows) + " raccourcis de la liste se réassignent au clic (dont Page précédente / suivante), boutons ‹ › avec le raccourci en infobulle"_t
                     + (failed.isEmpty() ? juce::String() : " [" + failed.joinIntoString (",") + "]"));
        }

        // Easter egg: three clicks on the logo within 800 ms unlock the hidden ZEYN theme (listed, not applied)
        const auto themeBefore = app.theme().id;
        main->sidebar.logoClicked (10000); main->sidebar.logoClicked (10500); main->sidebar.logoClicked (11000);
        const bool slowIgnored = ! app.isZeynUnlocked() && app.themes().findTheme ("zeyn") == nullptr;
        main->sidebar.logoClicked (20000); main->sidebar.logoClicked (20250); main->sidebar.logoClicked (20500);
        const auto* zeyn = app.themes().findTheme ("zeyn");
        t.check (slowIgnored && app.isZeynUnlocked() && (bool) app.settings().get ("zeynUnlocked", false) && zeyn != nullptr && zeyn->name == "ZEYN"
                 && app.theme().id == themeBefore && main->sidebar.logoBounds().getHeight() == 40,
                 "triple clic sur le logo (< 800 ms) : thème ZEYN déverrouillé et mémorisé, pas appliqué ; clics lents ignorés"_t);
        t.check (zeyn != nullptr && zeyn->bg() == juce::Colour (0xff1a1b1e) && zeyn->colour ("--nx-bg-2") == juce::Colour (0xff2c2e33)
                 && zeyn->accent() == juce::Colour (0xfff8fafc) && zeyn->textCol() == juce::Colours::white && zeyn->textDim() == juce::Colour (0xffcbd5e1)
                 && std::abs (zeyn->glassTint().getFloatAlpha() - 0.08f) < 0.01f && std::abs (zeyn->border().getFloatAlpha() - 0.28f) < 0.01f
                 && zeyn->number ("--nx-artwork-saturation", 1.0f) == 0.0f,
                 "thème ZEYN : verre givré monochrome (titane #1A1B1E → #2C2E33, verre blanc 8 %, bordure 28 %, accent #F8FAFC, pochette N&B)"_t);
        sw.getPage().setTab ("interface");
        bool listed = false;
        for (auto& th : app.themes().getThemes()) listed = listed || th.id == "zeyn";
        nx::ThemeManager fresh;
        fresh.setZeynUnlocked ((bool) app.settings().get ("zeynUnlocked", false));
        t.check (listed && fresh.findTheme ("zeyn") != nullptr, "Réglages › Apparence : ZEYN proposé parmi les thèmes, aussi après redémarrage"_t);
        {
            // crash regression (1.0.0, EXC_BAD_ACCESS in applyThemeNow): the "Thème" combo passed a string owned by its
            // own callback, and the settings page was rebuilt during the apply. The combo must outlive the change.
            juce::ComboBox* combo = nullptr;
            for (auto* c : sw.getPage().content().getChildren())
                if (auto* cb = dynamic_cast<juce::ComboBox*> (c))
                    for (int i = 0; i < cb->getNumItems(); ++i) if (cb->getItemText (i) == "Cobalt Minimal") combo = cb;
            juce::Component::SafePointer<juce::ComboBox> alive (combo);
            if (combo != nullptr) { combo->setText ("Cobalt Minimal", juce::dontSendNotification); combo->onChange(); }
            const bool survived = alive != nullptr && app.theme().id == "cobalt-minimal";
            settle (150);
            t.check (combo != nullptr && survived && alive == nullptr && app.theme().id == "cobalt-minimal",
                     "Réglages › Thème : changement de thème sans plantage (page reconstruite après le choix, pas pendant)"_t);
            app.applyTheme (themeBefore);      // also restores the remembered dark theme
            settle (50);
        }
        sw.close();
        app.applyThemeNow ("zeyn");
        app.navigate ({ nx::PageId::Albums });
        shoot ("26c_theme_zeyn");
        app.applyThemeNow (themeBefore);
        app.navigate ({ nx::PageId::Settings, "interface" });
        t.check (nx::shortcuts::describe (K ('m')) == "M" && nx::shortcuts::describe (K ('s', cmd | juce::ModifierKeys::shiftModifier, 0)).endsWith ("S")
                 && nx::shortcuts::describe (K (K::rightKey, cmd, 0)).endsWith (juce::String::fromUTF8 ("\xe2\x86\x92")),
                 "raccourcis affichés lisiblement (Ctrl / ⌘, flèches, lettres)"_t);
    }
    app.setRemoteEnabled (true);
    sw.getPage().setTab ("remote");
    t.check (app.remote().isRunning() && shoot ("27_nx_remote") > 30, "Aura Remote activé : QR code et lien"_t);
    {
        const auto state = juce::JSON::parse (app.buildRemoteState());
        t.check (state.getProperty ("family", {}).toString() == "merging" && state.getProperty ("profiles", {}).size() == 3,
                 "état Aura Remote : morceau, volume, profils Anubis ("_t + state.getProperty ("device", {}).toString() + ")");
        {
            // A / B from the phone: point A is reported as soon as it is set (the page shows "Point A posé à…")
            juce::StringPairArray none;
            app.handleRemoteCommand ("loopA", none);
            const double pending = (double) juce::JSON::parse (app.buildRemoteState()).getProperty ("loopPendingA", -1.0);
            app.handleRemoteCommand ("loopClear", none);
            const double cleared = (double) juce::JSON::parse (app.buildRemoteState()).getProperty ("loopPendingA", 0.0);
            t.check (pending >= 0.0 && cleared < 0.0, "Aura Remote A-B : point A transmis au téléphone dès qu'il est posé, effacé par Arrêt"_t);
        }
        {
            // Heart of the track being played: from the phone, from the player bar and from a list, always in step
            juce::StringPairArray none;
            auto stateFav = [&] { return (bool) juce::JSON::parse (app.buildRemoteState()).getProperty ("fav", false); };
            const auto* cur = app.currentTrack();
            const auto id = cur != nullptr ? cur->id : (juce::int64) 0;
            auto libFav = [&] { auto tr = app.library().getTrack (id); return tr && tr->favourite; };
            const bool before = libFav();
            const bool canFav = (bool) juce::JSON::parse (app.buildRemoteState()).getProperty ("canFav", false);
            app.handleRemoteCommand ("fav", none);                       // phone
            const bool phone = stateFav() == ! before && libFav() == ! before && main->player.favButton.active == ! before;
            main->player.favButton.onClick();                            // player bar
            const bool bar = stateFav() == before && libFav() == before && ! main->player.favButton.active == ! before;
            app.library().setFavourite (id, ! before);                   // a list (track heart, menu, drag to Favoris)
            const bool list = stateFav() == ! before && main->player.favButton.active == ! before;
            app.library().setFavourite (id, before);
            t.check (id > 0 && canFav && phone && bar && list && main->player.favButton.isVisible(),
                     "cœur du morceau en cours : téléphone, barre de lecture et listes synchronisés (bibliothèque, Aura Remote, icône)"_t);
        }
        juce::StringPairArray p;
        p.set ("v", "0.5");
        // the volume slider is greyed (and the command ignored) while the previous track still holds a fixed volume:
        // wait until the engine has moved on instead of racing it
        for (int i = 0; i < 40; ++i)
        {
            main->player.stateChanged();
            app.handleRemoteCommand ("volume", p);
            if (std::abs (app.getUserVolume() - 0.5) < 0.01) break;
            settle (100);
        }
        const double v = app.getUserVolume();
        p.set ("i", "2");
        app.handleRemoteCommand ("profile", p);
        const bool pureDop = app.engine().getSettings().anubisProfile == nx::AnubisProfile::PureDoP;
        p.set ("v", "1");
        // back to 100 % in the same turn (the slider is still enabled), then wait for the engine to adopt the DoP plan
        p.set ("v", "1");
        app.handleRemoteCommand ("volume", p);
        for (int i = 0; i < 60 && ! app.engine().isFixedVolume(); ++i) settle (100);
        t.check (std::abs (v - 0.5) < 0.01 && pureDop && app.engine().isFixedVolume(), "Aura Remote : volume à 50 %, profil Pure DoP depuis le téléphone, retour à 100 % bit-perfect"_t
                 + " [v=" + juce::String (v) + " dop=" + juce::String ((int) pureDop) + " fixed=" + juce::String ((int) app.engine().isFixedVolume()) + " state=" + juce::String ((int) app.engine().getState()) + " vol=" + juce::String (app.getUserVolume()) + "]");
        {
            const auto st = juce::JSON::parse (app.buildRemoteState());
            const bool dopNow = app.engine().getCurrentPlan().wire == nx::WireFormat::DoP;
            // 100 % bit-perfect is not a lock: in PCM / DXD the phone keeps its slider (it used to stay stuck at 100 %)
            t.check ((bool) st.getProperty ("volumeFixed", false) == (dopNow || app.engine().isVolumeLockedByProfile()),
                     "état Aura Remote : volume fixe seulement en DoP ou profil RME DSD Direct (pas à 100 % en PCM / DXD)"_t);
            t.check (! dopNow || (bool) st.getProperty ("dop", false), "état Aura Remote : DoP signalé quand la piste passe en DoP"_t);
        }
    }
    {
        // Step 5: covers for the phone are always a baseline RGB JPEG of at most 600 x 600, whatever the stored format
        auto asJpeg = [] (const juce::MemoryBlock& b) { return b.getSize() > 4 && ((const unsigned char*) b.getData())[0] == 0xff && ((const unsigned char*) b.getData())[1] == 0xd8; };
        bool allJpeg = true;
        for (auto& bytes : { std::make_pair (kWebp, sizeof (kWebp)), std::make_pair (kProgressiveJpeg, sizeof (kProgressiveJpeg)),
                             std::make_pair (kCmykJpeg, sizeof (kCmykJpeg)), std::make_pair (kPng16, sizeof (kPng16)) })
        {
            const auto key = app.library().storeArtwork (juce::MemoryBlock (bytes.first, bytes.second), "image/png");
            const auto jpg = app.remoteArtwork (key, 600);
            // baseline (SOF0), 8 bits, 3 components: read from the file itself, since the native decoders of macOS / Windows
            // hand back ARGB images whatever the JPEG holds
            const auto* d = (const unsigned char*) jpg.getData();
            bool baseline = false;
            for (size_t i = 2; i + 9 < jpg.getSize(); ++i)
                if (d[i] == 0xff && d[i + 1] == 0xc0) { baseline = d[i + 4] == 8 && d[i + 9] == 3; break; }
            const auto img = nx::imagedec::decode (jpg.getData(), jpg.getSize());
            allJpeg = allJpeg && asJpeg (jpg) && baseline && img.isValid() && img.getWidth() == 32;
        }
        t.check (allJpeg, "Aura Remote : WebP, JPEG progressif / CMYK et PNG 16 bits servis en JPEG RGB standard"_t);
        juce::Image big (juce::Image::ARGB, 1400, 900, true);
        { juce::Graphics g (big); g.fillAll (juce::Colours::orange); }
        juce::MemoryOutputStream png;
        juce::PNGImageFormat().writeImageToStream (big, png);
        const auto bigKey = app.library().storeArtwork (juce::MemoryBlock (png.getData(), png.getDataSize()), "image/png");
        const auto small = juce::ImageFileFormat::loadFrom (app.remoteArtwork (bigKey, 600).getData(), app.remoteArtwork (bigKey, 600).getSize());
        {
            // official logo: vector drawn from the embedded SVG (opaque centre, transparent margin), icon served to the phone
            juce::Image im (juce::Image::ARGB, 128, 128, true);
            { juce::Graphics g (im); nx::logo::draw (g, im.getBounds().toFloat()); }
            t.check (im.getPixelAt (64, 20).getAlpha() > 200 && im.getPixelAt (0, 0).getAlpha() < 20 && im.getPixelAt (40, 64).getBrightness() > 0.0f
                     && NXData::icon_pngSize > 1000 && ((const unsigned char*) NXData::icon_png)[1] == 'P',
                     "logo Aura : SVG vectoriel dessiné, icône Aura Remote embarquée"_t);
        }
        t.check (small.isValid() && small.getWidth() == 600 && small.getHeight() == 386, "Aura Remote : pochette réduite à 600 px max, proportions gardées"_t);
        // no key at all: the cover next to the file (cover / folder / front) is found through the track id
        const auto dir = t.tempDir.getChildFile ("remote-cover");
        dir.createDirectory();
        dir.getChildFile ("front.webp").replaceWithData (kWebp, sizeof (kWebp));
        auto pcm = sinePcm (4800, 2, 16, 48000.0, 440.0, 0.3);
        writeWav (dir.getChildFile ("t.wav"), &pcm, nullptr, 2, 48000.0, 16, 16, false);
        nx::TrackInfo ti;
        ti.source = "local"; ti.externalId = ti.uri = dir.getChildFile ("t.wav").getFullPathName(); ti.title = "Sans pochette"_t; ti.artist = "X"; ti.album = "Remote";
        const auto tid = app.library().upsert (ti);
        t.check (asJpeg (app.remoteArtwork ("track:" + juce::String (tid), 600)) && app.remoteArtwork ("inconnue.jpg", 600).getSize() == 0,
                 "Aura Remote : repli dossier (front.webp) via l'identifiant du morceau ; clé inconnue = aucune image"_t);
        // permanent link: the token and the port are saved, a new run reuses them
        const auto token = app.remote().getToken();
        t.check (app.settings().get ("remoteToken").toString() == token && token.length() >= 32, "Aura Remote : jeton enregistré dans les préférences"_t);
        const auto portBefore = app.remote().getPort();
        const auto urlBefore = app.remote().getUrl();
        app.setRemoteEnabled (false);
        app.setRemoteEnabled (true);
        t.check (app.remote().getPort() == portBefore && app.remote().getUrl() == urlBefore && (int) app.settings().get ("remotePort", 0) == portBefore,
                 "Aura Remote : arrêt puis redémarrage, même port, même lien, même QR code"_t);
        const auto oldUrl = app.remote().getUrl();
        app.remote().regenerateToken();
        t.check (app.settings().get ("remoteToken").toString() == app.remote().getToken() && app.remote().getUrl() != oldUrl, "Aura Remote : « Nouveau code » régénère le lien et l'enregistre"_t);
        {
            juce::String why;
            nx::RemoteServer fresh;
            fresh.setToken (app.remote().getToken());
            t.check (fresh.getToken() == app.remote().getToken() && fresh.start (portBefore, why) && fresh.getPort() > 0,
                     "Aura Remote : un serveur neuf reprend le jeton enregistré"_t);
        }
        const auto state2 = juce::JSON::parse (app.buildRemoteState());
        t.check (state2.hasProperty ("family") && state2.hasProperty ("position"), "état Aura Remote : famille de matériel (merging / rme / fiio)"_t);
    }
    app.setRemoteEnabled (false);
    sw.close();

    // Profile chip of the player bar
    {
        auto& chip = main->player.deviceChip;
        auto entries = chip.menuEntries();
        chip.selectEntry (1);
        const bool warm = app.engine().getSettings().anubisProfile == nx::AnubisProfile::AnalogWarmth;
        t.check (entries.size() == 3 && warm, "barre de lecture : miniature de l'Anubis et menu des 3 profils (Analog Warmth activé)"_t);
        chip.selectEntry (0);
    }

    // Studio tools from the player bar menu
    app.playTracks (neon, 0);
    settle (500);
    app.studioTools().monitor = nx::StudioToolsState::Monitor::Mono;
    app.studioTools().invertPhase = true;
    app.applyStudioTools();
    settle (300);
    {
        bool tools = false;
        for (auto& n : app.engine().getSignalPath().nodes) tools |= n.stage == "DSP" && n.title.contains ("Mono") && n.title.contains ("Phase");
        t.check (tools && app.engine().getSettings().dspNeedsPcm && main->player.studioButton.active,
                 "Outils Studio (mono + phase) : dans la chaîne et le chemin du signal, DSD converti tant qu'ils sont actifs"_t);
    }
    shoot ("28_outils_studio");
    app.studioTools().monitor = nx::StudioToolsState::Monitor::Stereo;
    app.studioTools().invertPhase = false;
    app.applyStudioTools();
    t.check (! app.engine().getSettings().dspNeedsPcm, "outils éteints : retour au bit-perfect"_t);

    // Immersive view, modular: theme (CSS + immersive.json), settings, plug-ins
    app.setImmersive (true);
    for (int i = 0; i < 40 && ! main->getImmersive().hasWaveform(); ++i) settle (50);
    t.check (main->isImmersive() && main->getImmersive().getPalette().size() == 4 && main->getImmersive().hasWaveform(),
             "vue immersive : 4 couleurs extraites de la pochette, forme d'onde calculée"_t);
    t.check (shoot ("29_vue_immersive") > 60, "vue immersive (fond animé, pochette HD, paroles, forme d'onde)"_t);
    if (app.currentTrack() != nullptr)
    {
        auto& heart = main->getImmersive().favButton;
        const bool before = app.currentTrack()->favourite;
        const bool shown = shownInMain (&heart);
        heart.triggerClick();
        settle (150);
        const bool toggled = app.currentTrack()->favourite != before && heart.active == ! before && main->player.favButton.active == ! before
                             && main->getNowPlaying().favButton.active == ! before;
        shoot ("29b_vue_immersive_coeur");
        heart.triggerClick();
        settle (150);
        t.check (shown && toggled && app.currentTrack()->favourite == before, "vue immersive : cœur près du titre, synchronisé avec la barre de lecture et « En cours »"_t
                 + " [" + juce::String ((int) shown) + juce::String ((int) toggled) + "]");
    }
    {
        auto folder = app.userDataDirectory().getChildFile ("Themes/immersive-test.nxtheme");
        folder.createDirectory();
        folder.getChildFile ("manifest.json").replaceWithText (R"({ "id": "immersive-test", "name": "Immersive Test", "stylesheet": "theme.css" })");
        folder.getChildFile ("theme.css").replaceWithText (":root { --nx-accent: #FF2A6D; --nx-immersive-layout: centered; --nx-immersive-vu: on; --nx-immersive-artwork-scale: 0.9; }");
        folder.getChildFile ("immersive.json").replaceWithText (R"({ "spectrum": true, "motion": 1.5 })");
        app.themes().rescan();
        app.applyThemeNow ("immersive-test");
        auto c = nx::ImmersiveConfig::resolve (app);
        const bool fromTheme = c.layout == "centered" && c.vu && c.spectrum && std::abs (c.motion - 1.5f) < 0.01f && c.source.contains ("th");
        main->getImmersive().refreshConfig();
        shoot ("30_vue_immersive_theme");
        auto userImm = juce::var (new juce::DynamicObject());
        userImm.getDynamicObject()->setProperty ("layout", "minimal");
        app.settings().set ("immersive", userImm);
        c = nx::ImmersiveConfig::resolve (app);
        const bool fromSettings = c.layout == "minimal" && c.vu;
        auto pluginImm = juce::var (new juce::DynamicObject());
        pluginImm.getDynamicObject()->setProperty ("lyrics", false);
        app.layout.getDynamicObject()->setProperty ("immersive", pluginImm);
        c = nx::ImmersiveConfig::resolve (app);
        const bool fromPlugin = ! c.lyrics && c.source == "plug-in";
        t.check (fromTheme && fromSettings && fromPlugin, "disposition modulable : thème (CSS + immersive.json), puis réglages, puis plug-ins"_t);
        app.layout.getDynamicObject()->removeProperty ("immersive");
        app.settings().set ("immersive", juce::var (new juce::DynamicObject()));
        app.applyThemeNow ("aura");
    }
    app.setImmersive (false);
    t.check (! main->isImmersive(), "Échap / bouton : sortie de la vue immersive"_t);

    // Onboarding assistant (first launch, or Réglages › Interface)
    app.showOnboarding();
    auto& ob = main->getOnboarding();
    t.check (ob.isVisible() && ob.getStep() == 0, "assistant de premier démarrage affiché"_t);
    shoot ("31_assistant_langue_theme");
    ob.clickTile ("lang", "en");
    const bool en = nx::i18n::isEnglish() && "Réglages"_t == "Settings";
    shoot ("32_assistant_english");
    ob.clickTile ("lang", "fr");
    t.check (en && ! nx::i18n::isEnglish(), "étape 1 : langue anglais / français appliquée à chaud"_t);
    {
        bool modeTiles = false, accentTiles = false;
        for (auto& tile : ob.getTiles()) { modeTiles = modeTiles || tile.kind == "mode"; accentTiles = accentTiles || tile.kind == "accent"; }
        const auto before = app.theme().id;
        ob.clickTile ("mode", "light");
        const bool light = app.theme().id == "pure-light";
        ob.clickTile ("accent", "3");
        const bool blue = ! app.getAccent().isTransparent();
        ob.clickTile ("mode", "dark");
        ob.clickTile ("accent", "0");
        t.check (modeTiles && accentTiles && light && blue && app.theme().id == before && app.getAccent().isTransparent(),
                 "étape 1 : apparence Sombre / Clair / Système et couleur d'accent (Personnalisez Aura)"_t);
    }
    ob.setStep (1);
    ob.clickTile ("layout", "compact");
    settle (100);
    const bool compact = main->sidebar.isCompact();
    shoot ("33_assistant_disposition");
    {
        bool density = false;
        for (auto& tile : ob.getTiles()) density = density || tile.kind == "density";
        t.check (density && app.settings().get ("layout").getProperty ("density", {}).toString() == "compact",
                 "étape 2 : dispositions Net / Minimal / Lecteur en haut, densité de la barre d'écoute (Minimal = compacte)"_t);
    }
    ob.clickTile ("layout", "net");
    t.check (compact && ! main->sidebar.isCompact(), "étape 2 : disposition Minimal (icônes) puis Net (étendue)"_t);
    ob.setStep (2);
    ob.clickTile ("profile", "0");
    t.check (app.engine().getSettings().anubisProfile == nx::AnubisProfile::StudioDxdReference, "étape 3 : Anubis détectée, profil optimal activé en un clic"_t);
    t.check (shoot ("34_assistant_materiel") > 40, "étape 3 : dessin de la Merging Anubis"_t);
    ob.setStep (3);
    shoot ("35_assistant_sources");
    ob.finish();
    t.check (! ob.isVisible() && (bool) app.settings().get ("onboardingDone", false), "fin de l'assistant : mémorisé (il ne revient plus seul)"_t);

    // Compact sidebar and the English interface
    app.setLayoutOption ("sidebar", "compact");
    app.setLanguage ("en");
    app.navigate ({ nx::PageId::Library, "albums" });
    t.check (shoot ("36_interface_anglais_compacte") > 40 && "Favoris"_t == "Favourites", "interface en anglais, barre latérale compacte"_t);
    app.setLanguage ("fr");
    app.setLayoutOption ("sidebar", "wide");
    virtualDac->name = originalDac;
    app.selectDevice (originalDac);

    // mini player
    nx::MiniPlayer mini (app);
    mini.setSize (380, 96);
    mini.trackChanged();
    settle (100);
    const auto miniImg = mini.createComponentSnapshot (mini.getLocalBounds());
    savePng (miniImg, shots.getChildFile ("11_mini_lecteur.png"));
    t.check (colourVariety (miniImg) > 20, "mini-lecteur");

    // Aura Free: Studio Master formats and Pro features ask for Premium (the same binary, the key removed)
    {
        auto dismiss = [&]
        {
            settle (50);
            while (auto* m = juce::ModalComponentManager::getInstance()->getModalComponent (0)) { m->exitModalState (0); settle (30); }
        };
        app.engine().stop();
        app.deactivateLicense();
        const auto s0 = app.engine().getSettings();
        t.check (! app.isPremium() && s0.freeEdition && app.settings().get ("licenseKey").toString().isEmpty()
                 && ! nx::resolveProfile (s0, "MT VAD (Merging Anubis Premium)").exclusive
                 && nx::activeFamily (s0, "MT VAD (Merging Anubis Premium)") == nx::DeviceFamily::None,
                 "Free : licence retirée, mode exclusif et profils matériels coupés dans le moteur"_t);

        std::vector<nx::TrackInfo> dsd, hiRes, cd;
        for (auto& tr : app.library().getTracks ("title", 1000))
            (tr.isDsd ? dsd : tr.sampleRate > 192000 ? hiRes : cd).push_back (tr);
        const int before = app.getUpsellCount();
        app.playTracks (dsd, 0);
        dismiss();
        const bool dsdBlocked = app.getUpsellCount() == before + 1 && app.getLastUpsell() == nx::UpsellReason::StudioMaster
                                && app.engine().getState() == nx::PlaybackEngine::State::Stopped;
        app.playTracks (hiRes, 0);
        dismiss();
        const bool dxdBlocked = app.getUpsellCount() == before + 2;
        // a mixed list: the Studio Master tracks are left out of the queue
        std::vector<nx::TrackInfo> mixed { cd.front(), dsd.front(), cd.back(), hiRes.front() };
        app.playTracks (mixed, 0);
        settle (200);
        bool onlyPcm = app.engine().getQueue().size() == 2;
        for (auto& q : app.engine().getQueue()) onlyPcm = onlyPcm && ! q.uri.endsWithIgnoreCase (".dsf") && ! q.uri.endsWithIgnoreCase (".dff");
        t.check (dsdBlocked && dxdBlocked && onlyPcm && ! dsd.empty() && ! hiRes.empty(),
                 "Free : DSD et DXD (> 192 kHz) bloqués avec la fenêtre Premium, PCM ≤ 192 kHz lu, file sans les Studio Master"_t);
        t.check (nx::AppController::upsellMessage (nx::UpsellReason::StudioMaster) == "Passez à Aura Premium pour débloquer la lecture haute résolution au-delà de 192 kHz."_t
                 && nx::AppController::upsellTitle (nx::UpsellReason::StudioMaster) == "Format Studio Master (DXD / DSD / SACD)"_t,
                 "fenêtre Premium : « Format Studio Master (DXD / DSD / SACD) — Passez à Aura Premium… »"_t);
        {
            app.playTracks (dsd, 0);
            settle (50);
            auto* w = dynamic_cast<juce::AlertWindow*> (juce::ModalComponentManager::getInstance()->getModalComponent (0));
            t.check (w != nullptr && w->getNumButtons() == 3 && w->getButton (0)->getButtonText() == "Entrer une clé de licence"_t
                     && w->getButton (1)->getButtonText() == "Acheter Aura Premium"_t,
                     "fenêtre Premium : boutons « Entrer une clé de licence » et « Acheter Aura Premium »"_t);
            shoot ("40_free_fenetre_premium");
            if (w != nullptr) w->exitModalState (1);                  // "enter a key" -> Réglages › À propos
            settle (100);
            t.check (main->getSettingsWindow().isOpen() && main->getSettingsWindow().getTab() == "apropos", "« Entrer une clé de licence » ouvre Réglages › À propos"_t);
            dismiss();
        }
        const int b2 = app.getUpsellCount();
        const bool remoteRefused = ! app.setRemoteEnabled (true) && ! app.remote().isRunning() && app.getLastUpsell() == nx::UpsellReason::Remote;
        dismiss();
        nx::actions::editAlbum (app, app.library().getAlbums ("title", 1).front());
        const bool editorRefused = app.getLastUpsell() == nx::UpsellReason::MetadataEditor && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 1;
        dismiss();
        t.check (remoteRefused && editorRefused && app.getUpsellCount() == b2 + 2, "Free : Aura Remote et éditeur de métadonnées réservés à Premium (fenêtre d'offre)"_t);

        // Réglages › À propos: the licence card, Free state, key typed in the field
        auto& sw2 = main->getSettingsWindow();
        app.navigate ({ nx::PageId::Settings, "apropos" });
        settle (100);
        juce::TextEditor* keyField = nullptr;
        for (auto* c : sw2.getPage().content().getChildren())
            for (auto* k : c->getChildren())
                if (auto* e = dynamic_cast<juce::TextEditor*> (k); e != nullptr && e->getTextToShowWhenEmpty().startsWith ("Collez")) keyField = e;
        t.check (keyField != nullptr && keyField->isVisible() && shoot ("41_licence_free") > 30, "Réglages › Licence & Mises à jour : édition Free, champ de clé"_t);
        if (keyField != nullptr)
        {
            keyField->setText ("nxp1-mauvaise-cle");
            keyField->onReturnKey();
            const bool rejected = ! app.isPremium();
            keyField->setText (juce::String (licensefixture::key ("Alex", "alex@example.com")).toLowerCase());   // typed in lower case
            keyField->onReturnKey();
            settle (150);
            t.check (rejected && app.isPremium() && app.license().getLicensee() == "Alex <alex@example.com>"
                     && app.settings().get ("licenseKey").toString().startsWith ("NXP1-"),
                     "clé collée dans les Réglages : mauvaise clé refusée, bonne clé acceptée (même en minuscules) et enregistrée"_t);
        }
        app.navigate ({ nx::PageId::Settings, "apropos" });
        settle (100);
        t.check (shoot ("42_licence_premium") > 30, "Réglages : « Aura Premium — Activé pour … », badge ÉDITION PREMIUM"_t);
        {
            // Licences & mentions légales: credits, button for the full Licences.txt (embedded), trademark notice
            juce::TextButton* licences = nullptr;
            for (auto* c : sw2.getPage().content().getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Afficher Licences.txt") licences = b;
            const auto text = juce::String::fromUTF8 (NXData::Licences_txt, NXData::Licences_txtSize);
            bool all = true;
            for (auto* needle : { "Raw Material Software", "Xiph.Org", "Jean-loup Gailly", "PNG Reference Library", "Independent JPEG Group",
                                  "HarfBuzz", "Muhammad Tayyab Akram", "Fabrice Bellard", "Monocypher", "Additional IP Rights Grant",
                                  "Sean Barrett", "Apache License", "SQLite", "Project Nayuki", "Steinberg", "CC BY-SA 4.0" })
                all = all && text.contains (needle);
            if (licences != nullptr) sw2.getPage().setScrollY (licences->getY() - 420);
            settle (100);
            t.check (licences != nullptr && all && shoot ("44_licences_mentions") > 30,
                     "À propos : licences et crédits, bouton Licences.txt (notices complètes embarquées), mention des marques"_t);
        }
        sw2.close();
        app.navigate ({ nx::PageId::Home });
        shoot ("43_barre_laterale_premium");
        {
            // launch animation: over the main component, fades in, hides itself after 1.5 s
            nx::SplashScreen splash (*main);
            settle (60);
            const bool covering = splash.isVisible() && splash.getBounds() == main->getLocalBounds() && ! splash.isFinished();
            const auto mid = main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f);
            settle (400);
            savePng (main->createComponentSnapshot (main->getLocalBounds(), true, 1.0f), shots.getChildFile ("00_demarrage.png"));
            for (int i = 0; i < 200 && ! splash.isFinished(); ++i) settle (10);
            t.check (covering && mid.isValid() && splash.isFinished() && ! splash.isVisible() && splash.progress() >= 1.0,
                     "écran de démarrage : par-dessus l'interface, fondu, fermé tout seul après 1,5 s"_t);
        }
        {
            // version.json announcing 1.0.1: the window offers the update, "Ignorer cette version" silences it
            const auto vf = t.tempDir.getChildFile ("update_ui.json");
            vf.replaceWithText (R"({ "latestVersion": "99.0.1", "releaseNotes": ["- Lecture plus rapide des DSF", "- Correctifs"],
                                     "downloadUrlMac": "", "downloadUrlWin": "", "downloadUrlLinux": "" })");
            const auto oldUrl = app.updates().getUrl();
            app.updates().setUrl (vf.getFullPathName());
            app.settings().set ("skippedVersion", juce::var());
            app.checkForUpdates (false);
            juce::AlertWindow* popup = nullptr;
            for (int i = 0; i < 200 && popup == nullptr; ++i)
            {
                settle (10);
                popup = dynamic_cast<juce::AlertWindow*> (juce::ModalComponentManager::getInstance()->getModalComponent (0));
            }
            const bool shown = popup != nullptr && popup->getNumButtons() == 3 && popup->getName().contains ("99.0.1");
            if (popup != nullptr) { savePng (popup->createComponentSnapshot (popup->getLocalBounds()), shots.getChildFile ("45_mise_a_jour.png")); popup->exitModalState (2); }   // "Ignorer cette version"
            settle (50);
            app.checkForUpdates (false);
            settle (400);
            const bool silenced = juce::ModalComponentManager::getInstance()->getModalComponent (0) == nullptr
                                  && app.settings().get ("skippedVersion").toString() == "99.0.1";
            dismiss();
            app.updates().setUrl (oldUrl);
            t.check (shown && silenced, "mise à jour disponible : fenêtre avec les nouveautés (Mettre à jour / Pas maintenant / Ignorer), version ignorée ensuite"_t);
        }
        // the key is read back at the next launch (it lives in the settings, outside the application)
        nx::LicenseManager next (licensefixture::publicKey());
        next.restore (app.settings().get ("licenseKey").toString());
        t.check (next.isPremium(), "licence relue au démarrage suivant (préférences utilisateur)"_t);
    }
    t.info ("captures : " + shots.getFullPathName());
    t.check (juce::String (nx::MediaControls::compiledBackend()).isNotEmpty(),
             "touches multimédia du système : "_t + juce::String::fromUTF8 (nx::MediaControls::compiledBackend()));
}
