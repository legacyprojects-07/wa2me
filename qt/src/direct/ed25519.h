// SHA-512, Ed25519 and XEdDSA — dependency-free C++17.
//
// WhatsApp's Signal layer uses XEdDSA: signatures are produced with the same
// Curve25519 identity key that is used for ECDH. The construction (as
// implemented by curve25519-js, which libsignal calls) is:
//   * clamp the 32-byte X25519 private key and use it as the Ed25519 seed
//   * derive the Ed25519 public key A from that seed
//   * sign normally (SHA-512 based Ed25519), then move the sign bit of A's
//     y-coordinate into bit 7 of signature byte 63
//   * verification reverses the bit move and converts the Montgomery public
//     key to its Edwards form (y = (u-1)/(u+1))
//
// This file also provides SHA-512 standalone, because the Signal protocol
// itself (HKDF-SHA512 is not used, but Ed25519 needs SHA-512) requires it.
//
// Verified against curve25519-js and Node's crypto in qt/tests/test_signal.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wa {
namespace crypto {

using Bytes = std::vector<uint8_t>;

Bytes sha512(const uint8_t *data, size_t len);
Bytes sha512(const Bytes &data);

// XEdDSA signature over `message` with a 32-byte X25519 private key.
// Returns 64 bytes.
Bytes xeddsaSign(const Bytes &privateKey, const Bytes &message);

// Verifies an XEdDSA signature against a 32-byte X25519 public key.
bool xeddsaVerify(const Bytes &publicKey, const Bytes &message, const Bytes &signature);

// Ed25519 public key (32 bytes) derived from a 32-byte seed.
Bytes ed25519PublicKeyFromSeed(const Bytes &seed);

// X25519 public key derived from a 32-byte private key (helper for callers
// that only keep the private half).
Bytes x25519PublicFromPrivate(const Bytes &privateKey);

} // namespace crypto
} // namespace wa
