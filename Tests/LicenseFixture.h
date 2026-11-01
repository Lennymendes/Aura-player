// A licence key pair for the test bench only (the shipped public key is the author's: its private key is not here).
#pragma once

#include "../Source/Core/LicenseFormat.h"

namespace licensefixture
{
inline void pair (nx::licensing::SecretKey& secret, nx::licensing::PublicKey& pub)
{
    std::array<uint8_t, 32> seed {};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = (uint8_t) (0x42 + i * 7);
    nx::licensing::keyPairFromSeed (seed, secret, pub);
}

inline nx::licensing::PublicKey publicKey() { nx::licensing::SecretKey s; nx::licensing::PublicKey p; pair (s, p); return p; }

inline std::string key (const std::string& name, const std::string& email, const std::string& edition = "premium")
{
    nx::licensing::SecretKey s; nx::licensing::PublicKey p;
    pair (s, p);
    nx::licensing::License l;
    l.edition = edition; l.name = name; l.email = email; l.date = "2026-11-01";
    return nx::licensing::signKey (l, s);
}
}
