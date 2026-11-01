// NX Player — MIT License
// Reassignable keyboard shortcuts (Réglages › Interface › Raccourcis clavier). Stored in the settings as
// { "shortcuts": { "playPause": "spacebar", "next": "command + cursor right", ... } } (juce::KeyPress text
// descriptions); keys a Stream Deck can send (any key + modifiers) work the same way.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppContext.h"
#include <array>
#include <optional>

namespace nx::shortcuts
{

enum class Action { PlayPause, Next, Previous, VolumeUp, VolumeDown, Mute, Shuffle, Repeat, Streamer, Search };
constexpr int numActions = 10;

struct Info
{
    Action action;
    const char* id;          // key in the settings
    const char* label;       // French source text (translated with tr())
    juce::KeyPress defaultKey;
};

const std::array<Info, numActions>& all();
const Info& info (Action a);

juce::KeyPress get (const AppSettings& s, Action a);
void set (AppSettings& s, Action a, const juce::KeyPress& key);
void resetAll (AppSettings& s);
/** The action bound to this key, if any. */
std::optional<Action> match (const AppSettings& s, const juce::KeyPress& key);
/** "⌘ + →", "Space"...: the key as shown in the settings. */
juce::String describe (const juce::KeyPress& key);

} // namespace nx::shortcuts
