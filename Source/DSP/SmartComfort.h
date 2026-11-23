// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Smart Ear Comfort: the mastering profile of a track from its analysis (DR, share of the 6-10 kHz energy), and the
// E9DS dose it gets when the mode is on. Bright / compressed master (DR <= 8 and treble >= -18 dB): E9DS Dynamic HF
// 8 kHz, -1,5 to -6 dB (more compression and more treble = more); dynamic (DR >= 12) or balanced: nothing (bit-perfect).
#pragma once

#include <juce_core/juce_core.h>
#include "../Core/Text.h"
#include <algorithm>
#include <cmath>

namespace nx::comfort
{

enum class Profile { Unknown, Bright, Balanced, Dynamic, Undetermined };   // Undetermined: measured, no DR (too short / silent)

struct Result { Profile profile = Profile::Unknown; double doseDb = 0.0; };

static constexpr double focusHz = 8000.0;

inline Result classify (int dr, double hfShareDb) noexcept
{
    if (! std::isfinite (hfShareDb)) return {};
    if (dr <= 0) return { Profile::Undetermined, 0.0 };
    if (dr <= 8 && hfShareDb >= -18.0)
        return { Profile::Bright, std::clamp (1.5 + 0.5 * (8 - dr) + 0.25 * (hfShareDb + 18.0), 1.5, 6.0) };
    return { dr >= 12 ? Profile::Dynamic : Profile::Balanced, 0.0 };
}

inline juce::String profileName (Profile p)
{
    switch (p)
    {
        case Profile::Bright:   return "Master brillant / compressé"_t;
        case Profile::Balanced: return "Mastering équilibré"_t;
        case Profile::Dynamic:  return "Enregistrement dynamique"_t;
        case Profile::Undetermined: return "Indéterminé (morceau trop court ou silencieux)"_t;
        case Profile::Unknown:  break;
    }
    return "Profil en cours de mesure"_t;
}

/** "DR6 · aigus -14,0 dB" */
inline juce::String measures (int dr, double hfShareDb)
{
    return "DR" + juce::String (dr) + juce::String::fromUTF8 (" \xc2\xb7 ") + "aigus "_t + juce::String (hfShareDb, 1).replaceCharacter ('.', ',') + " dB";
}

} // namespace nx::comfort
