// X25519 (Curve25519 Diffie-Hellman) — dependency-free C++17.
//
// WhatsApp's Noise handshake uses Curve25519 for the ephemeral and static
// key exchanges. Qt has no public-key crypto, and libsodium may not be
// available on Symbian, so this is a compact TweetNaCl-derived
// implementation (public-domain algorithm, 16-limb field arithmetic).
//
// Verified against RFC 7748 test vectors and Node's crypto.diffieHellman
// (see qt/tests/test_crypto.cpp).

#pragma once

#include <stdint.h>
#include <vector>

namespace wa {
namespace crypto {

using Bytes = std::vector<uint8_t>;

// Computes the X25519 shared secret from a 32-byte private scalar and a
// 32-byte u-coordinate public key.
bool x25519(const uint8_t scalar[32], const uint8_t point[32], uint8_t out[32]);

// Derives the public key for a private scalar (scalar * basepoint).
bool x25519Base(const uint8_t scalar[32], uint8_t out[32]);

struct CurveKeyPair {
    Bytes priv;   // 32 bytes
    Bytes pub;    // 32 bytes
};

// Generates a fresh X25519 key pair using randomBytes().
CurveKeyPair generateCurveKeyPair();

} // namespace crypto
} // namespace wa
