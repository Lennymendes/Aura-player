// NX Player — MIT License
// Offline licence keys, shared by the player (verification) and Tools/nx_keygen (signing). Standard library +
// Monocypher (Ed25519, RFC 8032) only.
//
//   NXP1-<payload>-<signature>
//     payload   = Base32 (RFC 4648, no padding) of "1|<edition>|<name>|<email>|<YYYY-MM-DD>" (UTF-8)
//     signature = Base32 of the 64-byte Ed25519 signature of "NXP1|" + that payload text
//
// The player only embeds the 32-byte public key: a key cannot be forged without the private key.
// Keys may be typed in lower case, with spaces or line breaks: they are normalised first.
#pragma once

#include "../../ThirdParty/monocypher/monocypher-ed25519.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nx::licensing
{

using PublicKey = std::array<uint8_t, 32>;
using SecretKey = std::array<uint8_t, 64>;   // Monocypher layout: 32-byte seed + public key

struct License
{
    std::string edition = "premium", name, email, date;
};

enum class Result { Ok, Malformed, BadSignature, UnknownEdition };

inline const char* base32Alphabet() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; }

inline std::string base32Encode (const uint8_t* data, size_t size)
{
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < size; ++i)
    {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) { out += base32Alphabet()[(buffer >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) out += base32Alphabet()[(buffer << (5 - bits)) & 31];
    return out;
}

inline bool base32Decode (const std::string& text, std::vector<uint8_t>& out)
{
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text)
    {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return false;
        buffer = (buffer << 5) | (uint32_t) v;
        bits += 5;
        if (bits >= 8) { out.push_back ((uint8_t) ((buffer >> (bits - 8)) & 0xFF)); bits -= 8; }
    }
    return (buffer & ((1u << bits) - 1)) == 0;    // leftover bits must be zero padding
}

/** Upper case, without spaces / line breaks / tabs. */
inline std::string normalizeKey (const std::string& key)
{
    std::string out;
    for (char c : key)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        out += (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
    }
    return out;
}

inline std::string makePayload (const License& l)
{
    return "1|" + l.edition + "|" + l.name + "|" + l.email + "|" + l.date;
}

inline bool parsePayload (const std::string& p, License& l)
{
    std::vector<std::string> f (1);
    for (char c : p) { if (c == '|') f.emplace_back(); else f.back() += c; }
    if (f.size() != 5 || f[0] != "1" || f[1].empty() || (f[2].empty() && f[3].empty())) return false;
    l.edition = f[1]; l.name = f[2]; l.email = f[3]; l.date = f[4];
    return true;
}

inline bool validField (const std::string& s) { return s.find ('|') == std::string::npos && s.size() < 200; }

/** Signs a licence. secret: from crypto_ed25519_key_pair. Empty string if a field is invalid. */
inline std::string signKey (const License& l, const SecretKey& secret)
{
    if (! validField (l.edition) || ! validField (l.name) || ! validField (l.email) || ! validField (l.date)) return {};
    const auto payload = makePayload (l);
    const auto message = "NXP1|" + payload;
    uint8_t sig[64];
    crypto_ed25519_sign (sig, secret.data(), reinterpret_cast<const uint8_t*> (message.data()), message.size());
    return "NXP1-" + base32Encode (reinterpret_cast<const uint8_t*> (payload.data()), payload.size()) + "-" + base32Encode (sig, 64);
}

inline Result verifyKey (const std::string& rawKey, const PublicKey& pub, License& out)
{
    const auto key = normalizeKey (rawKey);
    if (key.size() > 2000 || key.rfind ("NXP1-", 0) != 0) return Result::Malformed;
    const auto dash = key.find ('-', 5);
    if (dash == std::string::npos) return Result::Malformed;
    std::vector<uint8_t> payload, sig;
    if (! base32Decode (key.substr (5, dash - 5), payload) || ! base32Decode (key.substr (dash + 1), sig) || sig.size() != 64 || payload.empty())
        return Result::Malformed;
    const std::string text (payload.begin(), payload.end());
    const auto message = "NXP1|" + text;
    if (crypto_ed25519_check (sig.data(), pub.data(), reinterpret_cast<const uint8_t*> (message.data()), message.size()) != 0)
        return Result::BadSignature;
    License l;
    if (! parsePayload (text, l)) return Result::Malformed;
    if (l.edition != "premium") return Result::UnknownEdition;
    out = l;
    return Result::Ok;
}

/** 32 random bytes (the seed) -> key pair. The seed is wiped by Monocypher. */
inline void keyPairFromSeed (std::array<uint8_t, 32> seed, SecretKey& secret, PublicKey& pub)
{
    crypto_ed25519_key_pair (secret.data(), pub.data(), seed.data());
}

inline std::string toHex (const uint8_t* p, size_t n)
{
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

inline bool fromHex (const std::string& text, uint8_t* out, size_t n)
{
    std::string s;
    for (char c : text) if (! (c == ' ' || c == '\n' || c == '\r' || c == '\t')) s += c;
    if (s.size() != n * 2) return false;
    auto val = [] (char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < n; ++i)
    {
        const int a = val (s[2 * i]), b = val (s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t) (a * 16 + b);
    }
    return true;
}

} // namespace nx::licensing
