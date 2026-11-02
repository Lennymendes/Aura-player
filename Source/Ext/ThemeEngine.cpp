// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "ThemeEngine.h"
#include "../Core/Text.h"

namespace nx
{

//==============================================================================
std::map<juce::String, juce::String> parseCssVariables (const juce::String& cssIn)
{
    // strip /* comments */
    juce::String css;
    for (int i = 0; i < cssIn.length();)
    {
        if (cssIn.substring (i, i + 2) == "/*")
        {
            const int end = cssIn.indexOf (i + 2, "*/");
            if (end < 0) break;
            i = end + 2;
            continue;
        }
        css << cssIn[i];
        ++i;
    }

    std::map<juce::String, juce::String> raw;
    int pos = 0;
    while ((pos = css.indexOf (pos, "--")) >= 0)
    {
        const int colon = css.indexOfChar (pos, ':');
        const int semi = css.indexOfChar (pos, ';');
        const int brace = css.indexOfChar (pos, '}');
        int end = semi < 0 ? brace : (brace < 0 ? semi : juce::jmin (semi, brace));
        if (colon < 0 || end < 0 || colon > end) { pos += 2; continue; }
        const auto name = css.substring (pos, colon).trim();
        const auto value = css.substring (colon + 1, end).trim();
        if (name.containsOnly ("-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")) raw[name] = value;
        pos = end + 1;
    }

    // resolve var(--x, fallback)
    std::function<juce::String (const juce::String&, int)> resolve = [&] (const juce::String& v, int depth) -> juce::String
    {
        if (depth > 8 || ! v.contains ("var(")) return v;
        const int at = v.indexOf ("var(");
        int level = 0, close = -1;
        for (int i = at + 3; i < v.length(); ++i)
        {
            if (v[i] == '(') ++level;
            else if (v[i] == ')' && --level == 0) { close = i; break; }
        }
        if (close < 0) return v;
        const auto inner = v.substring (at + 4, close);
        const auto ref = inner.upToFirstOccurrenceOf (",", false, false).trim();
        const auto fallback = inner.contains (",") ? inner.fromFirstOccurrenceOf (",", false, false).trim() : juce::String();
        auto it = raw.find (ref);
        const auto replacement = it != raw.end() ? resolve (it->second, depth + 1) : resolve (fallback, depth + 1);
        return resolve (v.substring (0, at) + replacement + v.substring (close + 1), depth + 1);
    };

    std::map<juce::String, juce::String> out;
    for (auto& [k, v] : raw) out[k] = resolve (v, 0);
    return out;
}

bool parseCssColour (const juce::String& valueIn, juce::Colour& out)
{
    const auto v = valueIn.trim().toLowerCase();
    if (v.startsWithChar ('#'))
    {
        auto hex = v.substring (1);
        if (! hex.containsOnly ("0123456789abcdef")) return false;
        if (hex.length() == 3 || hex.length() == 4)
        {
            juce::String expanded;
            for (auto c : hex) { expanded << juce::String::charToString (c) << juce::String::charToString (c); }
            hex = expanded;
        }
        if (hex.length() == 6) { out = juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) hex.getHexValue32())); return true; }
        if (hex.length() == 8)
        {
            const auto rgba = (juce::uint32) hex.getHexValue32();
            out = juce::Colour ((juce::uint8) (rgba >> 24), (juce::uint8) (rgba >> 16), (juce::uint8) (rgba >> 8), (juce::uint8) rgba);
            return true;
        }
        return false;
    }
    if (v.startsWith ("rgb"))
    {
        auto args = juce::StringArray::fromTokens (v.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false), ",/ ", {});
        args.removeEmptyStrings();
        if (args.size() < 3) return false;
        auto channel = [] (const juce::String& s) { return (juce::uint8) juce::jlimit (0, 255, s.endsWithChar ('%') ? juce::roundToInt (s.getFloatValue() * 2.55f) : s.getIntValue()); };
        float a = 1.0f;
        if (args.size() >= 4) a = args[3].endsWithChar ('%') ? args[3].getFloatValue() / 100.0f : args[3].getFloatValue();
        out = juce::Colour (channel (args[0]), channel (args[1]), channel (args[2]), juce::jlimit (0.0f, 1.0f, a));
        return true;
    }
    if (v == "transparent") { out = juce::Colours::transparentBlack; return true; }
    const auto named = juce::Colours::findColourForName (v, juce::Colours::transparentWhite);
    if (named != juce::Colours::transparentWhite) { out = named; return true; }
    return false;
}

juce::Colour Theme::colour (const juce::String& name, juce::Colour fallback) const
{
    auto it = vars.find (name);
    juce::Colour c;
    return it != vars.end() && parseCssColour (it->second, c) ? c : fallback;
}

float Theme::number (const juce::String& name, float fallback) const
{
    auto it = vars.find (name);
    if (it == vars.end()) return fallback;
    const auto v = it->second.trim();
    if (v.isEmpty() || ! juce::CharacterFunctions::isDigit (v[0]) && v[0] != '.' && v[0] != '-') return fallback;
    return v.getFloatValue();   // "14px" -> 14, "0.6" -> 0.6
}

juce::String Theme::text (const juce::String& name, const juce::String& fallback) const
{
    auto it = vars.find (name);
    return it != vars.end() ? it->second.unquoted() : fallback;
}

//==============================================================================
ThemeManager::ThemeManager()
{
    // Built-in fallback so the UI always has values, even without theme files.
    active.id = "builtin";
    active.name = "NX Emerald (intégré)"_t;
    active.vars = parseCssVariables (":root { --nx-bg: #0A0C0E; --nx-accent: #00F5A0; --nx-accent-2: #00D9F5; }");
    startTimer (500);
}

ThemeManager::~ThemeManager() { stopTimer(); }

void ThemeManager::setSearchFolders (juce::Array<juce::File> f)
{
    searchFolders = std::move (f);
    rescan();
}

bool ThemeManager::loadTheme (const juce::File& folder, Theme& t, juce::String& error)
{
    const auto manifestFile = folder.getChildFile ("manifest.json");
    const auto manifest = juce::JSON::parse (manifestFile);
    if (! manifest.isObject()) { error = "manifest.json manquant ou invalide"_t; return false; }
    t = {};
    t.folder = folder;
    t.id = manifest.getProperty ("id", folder.getFileNameWithoutExtension()).toString();
    t.name = manifest.getProperty ("name", t.id).toString();
    t.author = manifest.getProperty ("author", {}).toString();
    t.version = manifest.getProperty ("version", "1.0").toString();
    t.description = manifest.getProperty ("description", {}).toString();
    t.background = manifest.getProperty ("background", {});
    const auto cssFile = folder.getChildFile (manifest.getProperty ("stylesheet", "theme.css").toString());
    if (! cssFile.existsAsFile()) { error = "feuille de style introuvable : "_t + cssFile.getFileName(); return false; }
    t.vars = parseCssVariables (cssFile.loadFileAsString());
    const auto shaderName = t.background.getProperty ("shader", {}).toString();
    if (shaderName.isNotEmpty()) t.shaderCode = folder.getChildFile (shaderName).loadFileAsString();
    return true;
}

void ThemeManager::rescan()
{
    std::map<juce::String, Theme> byId;
    for (auto& root : searchFolders)
        for (const auto& e : juce::RangedDirectoryIterator (root, false, "*", juce::File::findDirectories))
        {
            Theme t;
            juce::String err;
            if (loadTheme (e.getFile(), t, err)) byId[t.id] = t;
        }
    if (zeynUnlocked && byId.count ("zeyn") == 0) byId["zeyn"] = makeZeynTheme();
    themes.clear();
    for (auto& [id, t] : byId) themes.push_back (t);
}

Theme ThemeManager::makeZeynTheme()
{
    // Built in (no folder): monochrome frosted glass — titanium / platinum greys, translucent white cards, fine
    // specular white borders, white and silver text, an icy white accent. The artwork behind is shown in black & white.
    Theme t;
    t.id = "zeyn";
    t.name = "ZEYN";
    t.author = "NX Audio Suite";
    t.version = "2.0";
    t.description = "Thème caché : verre givré noir & blanc, gris titane, reflets argentés, accent blanc glacé."_t;
    t.background = juce::JSON::parse (R"({ "type": "artwork", "fallback": "mesh", "speed": 0.05 })");
    t.vars = parseCssVariables (R"CSS(:root {
        --nx-bg: #1A1B1E;
        --nx-bg-2: #2C2E33;
        --nx-surface: #232529;
        --nx-glass-tint: rgba(255, 255, 255, 0.08);
        --nx-glass-strong: rgba(44, 46, 51, 0.86);
        --nx-border: rgba(255, 255, 255, 0.28);
        --nx-highlight: rgba(255, 255, 255, 0.14);
        --nx-shadow: rgba(0, 0, 0, 0.55);
        --nx-text: #FFFFFF;
        --nx-text-dim: #CBD5E1;
        --nx-accent: #F8FAFC;
        --nx-accent-2: #CBD5E1;
        --nx-on-accent: #0B0C0E;
        --nx-quality-bitperfect: #E2E8F0;
        --nx-quality-enhanced: var(--nx-accent);
        --nx-quality-high: #CBD5E1;
        --nx-quality-low: #FBBF24;
        --nx-radius: 18px;
        --nx-radius-small: 10px;
        --nx-sidebar-width: 236px;
        --nx-player-height: 84px;
        --nx-player-position: bottom;
        --nx-card-size: 176px;
        --nx-artwork-blur: 64;
        --nx-artwork-dim: 0.58;
        --nx-artwork-saturation: 0.0;
        --nx-lyrics-active: #FFFFFF;
        --nx-lyrics-inactive: rgba(203, 213, 225, 0.30);
        --nx-lyrics-size: 30px;
        --nx-font: "";
        --nx-font-size: 14px;
        --nx-animation-speed: 1.0;
    })CSS");
    return t;
}

juce::StringArray ThemeManager::getThemeIds() const
{
    juce::StringArray ids;
    for (auto& t : themes) ids.add (t.id);
    return ids;
}

const Theme* ThemeManager::findTheme (const juce::String& id) const
{
    for (auto& t : themes) if (t.id == id) return &t;
    return nullptr;
}

bool ThemeManager::apply (const juce::String& id)
{
    const auto* t = findTheme (id);
    if (t == nullptr) return false;
    active = *t;
    activeFingerprint = active.folder.isDirectory() ? fingerprint (active.folder) : 0;   // built-in themes have no folder
    if (onThemeChanged) onThemeChanged (active);
    return true;
}

juce::int64 ThemeManager::fingerprint (const juce::File& folder) const
{
    juce::int64 f = 0;
    for (const auto& e : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFiles))
        f ^= e.getModificationTime().toMilliseconds() * 31 + e.getFileSize();
    return f;
}

void ThemeManager::timerCallback()
{
    if (active.folder == juce::File() || ! active.folder.isDirectory()) return;
    const auto fp = fingerprint (active.folder);
    if (fp == activeFingerprint) return;
    activeFingerprint = fp;
    Theme reloaded;
    juce::String err;
    if (loadTheme (active.folder, reloaded, err))
    {
        active = reloaded;
        for (auto& t : themes) if (t.id == active.id) t = active;
        if (onThemeChanged) onThemeChanged (active);
    }
}

juce::String ThemeManager::importTheme (const juce::File& source, juce::String& error)
{
    if (searchFolders.isEmpty()) { error = "aucun dossier de thèmes"_t; return {}; }
    const auto userFolder = searchFolders.getLast();
    userFolder.createDirectory();

    juce::File folder = source;
    juce::TemporaryFile temp;
    if (source.existsAsFile())     // zipped theme
    {
        juce::ZipFile zip (source);
        if (zip.getNumEntries() == 0) { error = "archive vide ou illisible"_t; return {}; }
        temp.getFile().createDirectory();
        zip.uncompressTo (temp.getFile());
        folder = temp.getFile();
        if (! folder.getChildFile ("manifest.json").existsAsFile())
            for (const auto& e : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findDirectories))
                if (e.getFile().getChildFile ("manifest.json").existsAsFile()) { folder = e.getFile(); break; }
    }

    Theme t;
    if (! loadTheme (folder, t, error)) return {};
    const auto dest = userFolder.getChildFile (juce::File::createLegalFileName (t.id) + ".nxtheme");
    dest.deleteRecursively();
    if (! folder.copyDirectoryTo (dest)) { error = "copie impossible"_t; return {}; }
    temp.getFile().deleteRecursively();
    rescan();
    return t.id;
}

bool ThemeManager::exportTheme (const juce::String& id, const juce::File& destination)
{
    const auto* t = findTheme (id);
    if (t == nullptr) return false;
    juce::ZipFile::Builder builder;
    for (const auto& e : juce::RangedDirectoryIterator (t->folder, true, "*", juce::File::findFiles))
        builder.addFile (e.getFile(), 9, e.getFile().getRelativePathFrom (t->folder));
    destination.deleteFile();
    juce::FileOutputStream out (destination);
    return out.openedOk() && builder.writeToStream (out, nullptr);
}

} // namespace nx
