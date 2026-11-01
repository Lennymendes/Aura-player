// NX Player — MIT License
// Free / Premium editions in one binary. Without a valid key the player runs as NX Player Free; an NXP1 key
// signed with the author's Ed25519 private key (Tools/nx_keygen) unlocks NX Player Premium. The key is kept in
// the user's settings (Application Support / AppData), so it survives every update of the application.
#pragma once

#include <juce_core/juce_core.h>
#include "LicenseFormat.h"

namespace nx
{

class LicenseManager
{
public:
    /** publicKey: the embedded one by default (tests use their own pair). */
    explicit LicenseManager (const licensing::PublicKey& publicKey);
    LicenseManager();

    /** Checks and adopts a key; false with a message when it is not valid. */
    bool activate (const juce::String& key, juce::String& error);
    void deactivate();
    /** A key read back from the settings at launch (invalid / foreign keys are ignored: Free). */
    void restore (const juce::String& storedKey);

    bool isPremium() const noexcept { return premium; }
    juce::String getKey() const { return key; }
    /** "Name <email>" (or just one of them). */
    juce::String getLicensee() const;
    const licensing::License& getLicense() const noexcept { return license; }

    /** Studio Master formats (Premium only): DSD, PCM above 192 kHz (DXD...), SACD images. */
    static bool isStudioMaster (bool isDsd, double sampleRate, const juce::String& uri);
    static constexpr double freeMaxRate = 192000.0;

private:
    licensing::PublicKey publicKey;
    bool premium = false;
    juce::String key;
    licensing::License license;
};

} // namespace nx
