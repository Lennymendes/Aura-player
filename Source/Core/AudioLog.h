// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Plain-text audio diagnostics (~/Library/Aura/audio.log on macOS). Message / control thread only
// (never from the real-time callback). Capped at 512 KB: the older half is dropped, the recent history stays.
#pragma once

#include <juce_core/juce_core.h>

namespace nx
{

inline juce::File audioLogFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Aura").getChildFile ("audio.log");
}

inline void audioLog (const juce::String& line)
{
    auto f = audioLogFile();
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 512 * 1024)
    {
        const auto all = f.loadFileAsString();
        f.replaceWithText (all.substring (all.length() / 2).fromFirstOccurrenceOf ("\n", false, false));
    }
    f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  " + line + "\n");
}

} // namespace nx
