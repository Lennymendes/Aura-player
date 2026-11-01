// NX Player — MIT License
#include "UpdateChecker.h"
#include "Text.h"
#include <thread>

namespace nx
{

juce::String UpdateInfo::downloadUrlForThisOS() const
{
   #if JUCE_MAC
    return downloadUrlMac;
   #elif JUCE_WINDOWS
    return downloadUrlWin;
   #else
    return downloadUrlLinux;
   #endif
}

UpdateChecker::UpdateChecker (juce::String v) : current (std::move (v)) {}
UpdateChecker::~UpdateChecker() { alive->store (false); }

bool UpdateChecker::parse (const juce::String& json, UpdateInfo& out, juce::String& error)
{
    juce::var v;
    if (json.trim().isEmpty() || juce::JSON::parse (json, v).failed() || ! v.isObject()) { error = "fichier de version illisible"_t; return false; }
    UpdateInfo i;
    i.latestVersion = v.getProperty ("latestVersion", {}).toString().trim();
    i.releaseNotes = v.getProperty ("releaseNotes", {}).toString();
    i.downloadUrlMac = v.getProperty ("downloadUrlMac", {}).toString();
    i.downloadUrlWin = v.getProperty ("downloadUrlWin", {}).toString();
    i.downloadUrlLinux = v.getProperty ("downloadUrlLinux", {}).toString();
    i.mandatory = (bool) v.getProperty ("mandatory", false);
    if (i.latestVersion.isEmpty() || ! juce::CharacterFunctions::isDigit (i.latestVersion.trimCharactersAtStart ("vV")[0]))
    {
        error = "numéro de version absent"_t;
        return false;
    }
    out = i;
    return true;
}

int UpdateChecker::compareVersions (const juce::String& aIn, const juce::String& bIn)
{
    auto split = [] (const juce::String& s, juce::StringArray& nums, juce::String& pre)
    {
        auto t = s.trim().trimCharactersAtStart ("vV").upToFirstOccurrenceOf ("+", false, false);   // build metadata ignored
        pre = t.fromFirstOccurrenceOf ("-", false, false);
        nums = juce::StringArray::fromTokens (t.upToFirstOccurrenceOf ("-", false, false), ".", {});
    };
    juce::StringArray na, nb;
    juce::String pa, pb;
    split (aIn, na, pa);
    split (bIn, nb, pb);
    for (int i = 0; i < juce::jmax (3, na.size(), nb.size()); ++i)
    {
        const auto x = na[i].getLargeIntValue(), y = nb[i].getLargeIntValue();
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa.isEmpty() != pb.isEmpty()) return pa.isEmpty() ? 1 : -1;    // a release beats its pre-releases
    if (pa == pb) return 0;
    // pre-release identifiers, dot by dot: numbers numerically, below text
    auto ia = juce::StringArray::fromTokens (pa, ".", {}), ib = juce::StringArray::fromTokens (pb, ".", {});
    for (int i = 0; i < juce::jmin (ia.size(), ib.size()); ++i)
    {
        const bool da = ia[i].containsOnly ("0123456789"), db = ib[i].containsOnly ("0123456789");
        if (da && db) { const auto x = ia[i].getLargeIntValue(), y = ib[i].getLargeIntValue(); if (x != y) return x < y ? -1 : 1; continue; }
        if (da != db) return da ? -1 : 1;
        if (const int c = ia[i].compare (ib[i]); c != 0) return c < 0 ? -1 : 1;
    }
    return ia.size() == ib.size() ? 0 : (ia.size() < ib.size() ? -1 : 1);
}

UpdateChecker::Status UpdateChecker::evaluate (const juce::String& json, const juce::String& currentVersion)
{
    Status s;
    s.checkedAt = juce::Time::getCurrentTime();
    if (! parse (json, s.info, s.error)) { s.state = State::Failed; return s; }
    s.state = compareVersions (s.info.latestVersion, currentVersion) > 0 ? State::Available : State::UpToDate;
    return s;
}

void UpdateChecker::checkNow (std::function<void (const Status&)> done)
{
    if (status.state == State::Checking) return;
    status.state = State::Checking;
    const auto u = url;
    const auto version = current;
    auto token = alive;
    std::thread ([this, u, version, token, done]
    {
        juce::String body, error;
        const juce::URL target = juce::File::isAbsolutePath (u) ? juce::URL (juce::File (u)) : juce::URL (u);
        if (auto in = target.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                    .withConnectionTimeoutMs (8000).withNumRedirectsToFollow (5)))
            body = in->readEntireStreamAsString();
        else error = "serveur de mises à jour injoignable"_t;
        auto result = error.isNotEmpty() ? Status {} : evaluate (body, version);
        if (error.isNotEmpty()) { result.state = State::Failed; result.error = error; result.checkedAt = juce::Time::getCurrentTime(); }
        juce::MessageManager::callAsync ([this, token, result, done]
        {
            if (! token->load()) return;                 // the checker is gone
            status = result;
            if (done) done (status);
        });
    }).detach();
}

} // namespace nx
