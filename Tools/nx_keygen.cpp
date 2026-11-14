// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Licence key generator for the author of Aura (keep the private key out of the repository).
//
//   nx_keygen new-keypair <private.key>                         creates a key pair, prints the public key
//   nx_keygen sign <private.key> "<name>" "<email>" [date]      prints an NXP1-... Premium key (date: YYYY-MM-DD)
//   nx_keygen verify <public key hex> <NXP1-...>                checks a key
//
// The public key printed by new-keypair goes into Source/Core/LicensePublicKey.h.
#include "../Source/Core/LicenseFormat.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

using namespace nx::licensing;

static int usage()
{
    std::cerr << "usage:\n"
                 "  nx_keygen new-keypair <private.key>\n"
                 "  nx_keygen sign <private.key> \"<name>\" \"<email>\" [YYYY-MM-DD]\n"
                 "  nx_keygen verify <public key hex> <NXP1-...>\n";
    return 2;
}

static bool loadSecret (const std::string& path, SecretKey& secret, PublicKey& pub)
{
    std::ifstream f (path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::array<uint8_t, 32> seed {};
    if (! f || ! fromHex (ss.str(), seed.data(), 32)) return false;
    keyPairFromSeed (seed, secret, pub);
    return true;
}

int main (int argc, char** argv)
{
    if (argc < 2) return usage();
    const std::string cmd = argv[1];

    if (cmd == "new-keypair" && argc == 3)
    {
        std::random_device rd;                       // OS entropy (/dev/urandom, BCryptGenRandom, arc4random)
        std::array<uint8_t, 32> seed {};
        for (auto& b : seed) b = (uint8_t) (rd() & 0xFF);
        const auto hex = toHex (seed.data(), 32);
        SecretKey secret; PublicKey pub;
        keyPairFromSeed (seed, secret, pub);
        std::ofstream out (argv[2]);
        out << hex << "\n";
        if (! out) { std::cerr << "cannot write " << argv[2] << "\n"; return 1; }
        std::cout << "private key (seed) written to " << argv[2] << " — keep it secret, never commit it\n"
                  << "public key: " << toHex (pub.data(), 32) << "\n";
        return 0;
    }

    if (cmd == "sign" && (argc == 5 || argc == 6))
    {
        SecretKey secret; PublicKey pub;
        if (! loadSecret (argv[2], secret, pub)) { std::cerr << "invalid private key file\n"; return 1; }
        License l;
        l.name = argv[3];
        l.email = argv[4];
        if (argc == 6) l.date = argv[5];
        else
        {
            const std::time_t now = std::time (nullptr);
            char buf[16];
            std::strftime (buf, sizeof (buf), "%Y-%m-%d", std::gmtime (&now));
            l.date = buf;
        }
        const auto key = signKey (l, secret);
        if (key.empty()) { std::cerr << "fields may not contain '|'\n"; return 1; }
        License back;
        if (verifyKey (key, pub, back) != Result::Ok) { std::cerr << "self-check failed\n"; return 1; }
        std::cout << key << "\n";
        return 0;
    }

    if (cmd == "verify" && argc == 4)
    {
        PublicKey pub {};
        if (! fromHex (argv[2], pub.data(), 32)) { std::cerr << "invalid public key\n"; return 1; }
        License l;
        const auto r = verifyKey (argv[3], pub, l);
        if (r != Result::Ok) { std::cout << (r == Result::BadSignature ? "INVALID signature\n" : "MALFORMED key\n"); return 1; }
        std::cout << "valid " << l.edition << " licence: " << l.name << " <" << l.email << "> " << l.date << "\n";
        return 0;
    }
    return usage();
}
