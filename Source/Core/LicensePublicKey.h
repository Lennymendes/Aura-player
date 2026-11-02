// NX Player — Copyright (c) 2026 Lenny Antunes Mendes (NX Audio). Proprietary, all rights reserved (see LICENSE).
// Public half of the licence signing key (Ed25519). Only this is shipped: the private key stays with the author
// (Tools/nx_keygen). To use your own pair: nx_keygen new-keypair my.key, then paste its public key here.
#pragma once

#include "LicenseFormat.h"

namespace nx::licensing
{
inline const PublicKey& embeddedPublicKey()
{
    // 46ec0f8ac0d329ec15ca78daa041a0d36495840798bd09cddf8ed4941c33e0a4
    static const PublicKey key {{
    0x46, 0xec, 0x0f, 0x8a, 0xc0, 0xd3, 0x29, 0xec,
    0x15, 0xca, 0x78, 0xda, 0xa0, 0x41, 0xa0, 0xd3,
    0x64, 0x95, 0x84, 0x07, 0x98, 0xbd, 0x09, 0xcd,
    0xdf, 0x8e, 0xd4, 0x94, 0x1c, 0x33, 0xe0, 0xa4
    }};
    return key;
}
}
