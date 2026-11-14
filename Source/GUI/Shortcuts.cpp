// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Shortcuts.h"
#include "../Core/Text.h"

namespace nx::shortcuts
{

const std::array<Info, numActions>& all()
{
    using K = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    static const std::array<Info, numActions> list
    {{
        { Action::PlayPause,  "playPause",  "Lecture / Pause",                         K (K::spaceKey) },
        { Action::Next,       "next",       "Piste suivante",                          K (K::rightKey, cmd, 0) },
        { Action::Previous,   "previous",   "Piste précédente",                        K (K::leftKey, cmd, 0) },
        { Action::VolumeUp,   "volumeUp",   "Monter le volume",                        K (K::upKey, cmd, 0) },
        { Action::VolumeDown, "volumeDown", "Baisser le volume",                       K (K::downKey, cmd, 0) },
        { Action::Mute,       "mute",       "Muet",                                    K ('m') },
        { Action::Shuffle,    "shuffle",    "Activer / désactiver la lecture aléatoire", K ('s') },
        { Action::Repeat,     "repeat",     "Changer le mode Répéter",                 K ('r') },
        { Action::Streamer,   "streamer",   "Activer / désactiver le mode Streamer",   K ('s', cmd | juce::ModifierKeys::shiftModifier, 0) },
        { Action::Search,     "search",     "Rechercher dans la bibliothèque",         K ('f', cmd, 0) },
    }};
    return list;
}

const Info& info (Action a) { return all()[(size_t) a]; }

juce::KeyPress get (const AppSettings& s, Action a)
{
    const auto stored = s.get ("shortcuts").getProperty (info (a).id, {}).toString();
    if (stored == "-") return {};                                   // explicitly unassigned
    if (stored.isNotEmpty())
        if (auto k = juce::KeyPress::createFromDescription (stored); k.isValid()) return k;
    return info (a).defaultKey;
}

void set (AppSettings& s, Action a, const juce::KeyPress& key)
{
    auto* o = new juce::DynamicObject();
    if (auto* old = s.get ("shortcuts").getDynamicObject())
        for (auto& p : old->getProperties()) o->setProperty (p.name, p.value);
    o->setProperty (info (a).id, key.isValid() ? key.getTextDescription() : juce::String ("-"));
    s.set ("shortcuts", juce::var (o));
}

void resetAll (AppSettings& s) { s.set ("shortcuts", juce::var (new juce::DynamicObject())); }

std::optional<Action> match (const AppSettings& s, const juce::KeyPress& key)
{
    for (auto& i : all())
        if (const auto k = get (s, i.action); k.isValid() && k == key) return i.action;
    return std::nullopt;
}

juce::String describe (const juce::KeyPress& key)
{
    auto u = [] (const char* utf8) { return juce::String::fromUTF8 (utf8); };
    if (! key.isValid()) return u ("\xe2\x80\x94");                                    // —
    const auto m = key.getModifiers();
    juce::StringArray parts;
   #if JUCE_MAC
    if (m.isCtrlDown())    parts.add (u ("\xe2\x8c\x83"));                                // ⌃
    if (m.isAltDown())     parts.add (u ("\xe2\x8c\xa5"));                                // ⌥
    if (m.isShiftDown())   parts.add (u ("\xe2\x87\xa7"));                                // ⇧
    if (m.isCommandDown()) parts.add (u ("\xe2\x8c\x98"));                                // ⌘
   #else
    if (m.isCtrlDown())    parts.add ("Ctrl");
    if (m.isAltDown())     parts.add ("Alt");
    if (m.isShiftDown())   parts.add ("Maj"_t);
   #endif
    const int code = key.getKeyCode();
    juce::String name;
    if (code == juce::KeyPress::spaceKey)      name = "Espace"_t;
    else if (code == juce::KeyPress::rightKey) name = u ("\xe2\x86\x92");                // →
    else if (code == juce::KeyPress::leftKey)  name = u ("\xe2\x86\x90");                // ←
    else if (code == juce::KeyPress::upKey)    name = u ("\xe2\x86\x91");                // ↑
    else if (code == juce::KeyPress::downKey)  name = u ("\xe2\x86\x93");                // ↓
    else if (code == juce::KeyPress::returnKey) name = "Entrée"_t;
    else if (code == juce::KeyPress::tabKey)   name = "Tab";
    else if (code < 128 && juce::CharacterFunctions::isLetterOrDigit ((juce::juce_wchar) code))
        name = juce::String::charToString (juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code));
    else name = juce::KeyPress (code).getTextDescription().toUpperCase();      // F5, page up, punctuation...
    parts.add (name);
    return parts.joinIntoString (" + ");
}

} // namespace nx::shortcuts
