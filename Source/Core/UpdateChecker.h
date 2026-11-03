// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Update check, the same for Free and Premium (one binary; the licence stays in the settings across updates).
// Reads a small JSON file (default: version.json of the public repository Lennymendes/nx-audio-releases):
//   { "latestVersion": "1.1.0", "releaseNotes": ["line", "line"] or "text", "releaseNotesEn": [...] (optional),
//     "downloadUrlMac": "...", "downloadUrlWin": "...", "downloadUrlLinux": "...", "mandatory": false }
// The download itself is left to the browser (the installer for the right OS).
#pragma once

#include <juce_events/juce_events.h>
#include <atomic>
#include <functional>
#include <memory>

namespace nx
{

struct UpdateInfo
{
    juce::String latestVersion, releaseNotes, releaseNotesEn, downloadUrlMac, downloadUrlWin, downloadUrlLinux;
    bool mandatory = false;
    /** The installer for this operating system (empty if the file has none). */
    juce::String downloadUrlForThisOS() const;
};

class UpdateChecker
{
public:
    static constexpr const char* defaultUrl = "https://raw.githubusercontent.com/Lennymendes/nx-audio-releases/main/version.json";

    enum class State { Idle, Checking, UpToDate, Available, Failed };
    struct Status
    {
        State state = State::Idle;
        UpdateInfo info;
        juce::String error;
        juce::Time checkedAt;
    };

    explicit UpdateChecker (juce::String currentVersion);
    ~UpdateChecker();

    void setUrl (const juce::String& u) { url = u; }
    juce::String getUrl() const { return url; }
    juce::String getCurrentVersion() const { return current; }
    Status getStatus() const { return status; }

    /** Fetches the JSON on a background thread; `done` is called on the message thread. */
    void checkNow (std::function<void (const Status&)> done = {});

    /** Parses version.json; false (with a reason) when the file is not usable. */
    static bool parse (const juce::String& json, UpdateInfo& out, juce::String& error);
    /** Semantic versions: < 0, 0, > 0 (a leading "v" is ignored, 1.0.0-beta < 1.0.0, 1.10 > 1.9). */
    static int compareVersions (const juce::String& a, const juce::String& b);
    /** The status for a fetched file (pure: used by checkNow and the tests). */
    static Status evaluate (const juce::String& json, const juce::String& currentVersion);

private:
    juce::String current, url { defaultUrl };
    Status status;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace nx
