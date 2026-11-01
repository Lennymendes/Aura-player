// NX Player — MIT License
#include "LicenseManager.h"
#include "LicensePublicKey.h"
#include "Text.h"

namespace nx
{

LicenseManager::LicenseManager (const licensing::PublicKey& k) : publicKey (k) {}
LicenseManager::LicenseManager() : publicKey (licensing::embeddedPublicKey()) {}

bool LicenseManager::activate (const juce::String& candidate, juce::String& error)
{
    licensing::License l;
    switch (licensing::verifyKey (candidate.toStdString(), publicKey, l))
    {
        case licensing::Result::Ok: break;
        case licensing::Result::BadSignature:   error = "Clé de licence non reconnue (signature invalide)"_t; return false;
        case licensing::Result::UnknownEdition: error = "Clé de licence pour une autre édition"_t; return false;
        case licensing::Result::Malformed:      error = "Clé de licence mal formée (elle commence par NXP1-)"_t; return false;
    }
    premium = true;
    license = l;
    key = juce::String (licensing::normalizeKey (candidate.toStdString()));
    return true;
}

void LicenseManager::deactivate()
{
    premium = false;
    key = {};
    license = {};
}

void LicenseManager::restore (const juce::String& storedKey)
{
    juce::String ignored;
    if (storedKey.isEmpty() || ! activate (storedKey, ignored)) deactivate();
}

juce::String LicenseManager::getLicensee() const
{
    const auto name = juce::String::fromUTF8 (license.name.c_str()), email = juce::String::fromUTF8 (license.email.c_str());
    if (name.isNotEmpty() && email.isNotEmpty()) return name + " <" + email + ">";
    return name.isNotEmpty() ? name : email;
}

bool LicenseManager::isStudioMaster (bool isDsd, double sampleRate, const juce::String& uri)
{
    return isDsd || sampleRate > freeMaxRate + 0.5 || uri.containsIgnoreCase (".iso#area=") || uri.endsWithIgnoreCase (".iso");
}

} // namespace nx
