// SHA-256, HMAC-SHA256 and HKDF-SHA256 — dependency-free C++17.
//
// The Noise handshake needs SHA-256 (handshake hash), HKDF-SHA256 (key
// derivation, exactly as Baileys' `hkdf(data, 64, {salt, info})`) and
// HMAC-SHA256 (media MAC). Qt offers QCryptographicHash, but that is not
// available on Qt 4.7/Symbian and pulls in no HKDF, so this small
// implementation keeps the direct-mode stack portable.
//
// Verified against Node's crypto module (see qt/tests/test_crypto.cpp).

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wa {
namespace crypto {

using Bytes = std::vector<uint8_t>;

Bytes sha256(const uint8_t *data, size_t len);
Bytes sha256(const Bytes &data);
Bytes sha256(const std::string &data);

Bytes hmacSha256(const Bytes &key, const Bytes &data);

// HKDF-SHA256 (RFC 5869): extract with `salt`, expand with `info`.
Bytes hkdfSha256(const Bytes &ikm, size_t length,
                 const Bytes &salt = Bytes(), const Bytes &info = Bytes());

// Constant-time compare; returns true when equal.
bool constantTimeEquals(const Bytes &a, const Bytes &b);

// Cryptographically secure random bytes. Uses std::random_device plus a
// SHA-256-based DRBG over Qt-free entropy sources; see note in the .cpp.
Bytes randomBytes(size_t n);

std::string toHex(const Bytes &b);
Bytes fromHex(const std::string &hex);

} // namespace crypto
} // namespace wa
