// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Bug reports from users: a crash log (Windows / Linux; macOS keeps its own, richer .ips reports) and one zip with
// everything support needs (Réglages › À propos › Exporter les journaux). Nothing private goes in it: no licence key,
// no settings file (it holds service tokens), no library paths — only the logs and a short system summary.
#pragma once

#include "AudioLog.h"

namespace nx::diagnostics
{

inline juce::File crashLogFile() { return audioLogFile().getSiblingFile ("crash.log"); }

/** Appends the stack of an unexpected crash to crash.log. Not on macOS: JUCE's handler ends the process with SIGKILL,
    which would replace the system crash report (full symbolicated stack) by a useless one. */
inline void installCrashHandler()
{
   #if ! JUCE_MAC
    juce::SystemStats::setApplicationCrashHandler ([] (void*)
    {
        // shortcut: allocates inside a crash handler (not async-signal-safe); fine for most crashes, a minidump writer if it is not
        auto f = crashLogFile();
        f.getParentDirectory().createDirectory();
        f.appendText ("=== " + juce::Time::getCurrentTime().toISO8601 (true) + "  Aura " NX_VERSION "  "
                      + juce::SystemStats::getOperatingSystemName() + "\n" + juce::SystemStats::getStackBacktrace() + "\n");
    });
   #endif
}

/** The crash reports macOS wrote for Aura (or for NX Player, its former name), newest first. */
inline juce::Array<juce::File> systemCrashReports (int max = 5)
{
    juce::Array<juce::File> found;
   #if JUCE_MAC
    const auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/DiagnosticReports");
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
        if (f.getFileName().startsWith ("Aura") || f.getFileName().startsWith ("NX Player")) found.add (f);
    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    found.removeRange (max, found.size());
   #else
    juce::ignoreUnused (max);
   #endif
    return found;
}

/** Newest crash Aura knows of (crash.log or a macOS report); Time() when none. */
inline juce::Time lastCrashTime()
{
    juce::Time t;
    if (crashLogFile().existsAsFile()) t = crashLogFile().getLastModificationTime();
    for (auto& f : systemCrashReports (1)) t = juce::jmax (t, f.getLastModificationTime());
    return t;
}

/** Writes Aura-journaux-<date>.zip into folder: diagnostic.txt (summary), audio.log, crash.log, macOS crash reports.
    Returns the zip, or File() when it cannot be written. */
inline juce::File exportZip (const juce::String& summary, const juce::File& folder)
{
    juce::ZipFile::Builder zip;
    zip.addEntry (new juce::MemoryInputStream (summary.toRawUTF8(), summary.getNumBytesAsUTF8(), true), 9, "diagnostic.txt", juce::Time::getCurrentTime());
    for (auto& f : { audioLogFile(), crashLogFile() })
        if (f.existsAsFile()) zip.addFile (f, 9, f.getFileName());
    for (auto& f : systemCrashReports())
        zip.addFile (f, 9, "macOS/" + f.getFileName());

    folder.createDirectory();
    const auto out = folder.getNonexistentChildFile ("Aura-journaux-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M"), ".zip", false);
    {
        juce::FileOutputStream os (out);
        if (! os.openedOk() || ! zip.writeToStream (os, nullptr)) { out.deleteFile(); return {}; }
    }
    return out;
}

} // namespace nx::diagnostics
