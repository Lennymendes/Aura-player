// NX Player — Copyright (c) 2026 Lenny Antunes Mendes (NX Audio). Proprietary, all rights reserved (see LICENSE).
// Plain-text audio diagnostics (~/Library/NX Player/audio.log on macOS). Message / control thread only
// (never from the real-time callback). Capped at 512 KB.
#pragma once

#include <juce_core/juce_core.h>

namespace nx
{

inline juce::File audioLogFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("NX Player").getChildFile ("audio.log");
}

inline void audioLog (const juce::String& line)
{
    auto f = audioLogFile();
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 512 * 1024) f.deleteFile();
    f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  " + line + "\n");
}

} // namespace nx
