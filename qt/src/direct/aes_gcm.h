// AES-256-GCM and AES-256-CBC — dependency-free C++17.
//
// The Noise transport in WhatsApp uses AES-256-GCM with a 12-byte IV and the
// handshake hash as additional authenticated data. Media encryption uses
// AES-256-CBC + HMAC-SHA256. Both are needed, and neither is available in
// Qt 4.7 / Symbian, so this file provides them without external libraries.
//
// The S-box is derived at start-up from GF(2^8) arithmetic rather than pasted
// as a table, which removes table-typo risk; correctness is verified against
// NIST GCM test vectors and Node's crypto in qt/tests/test_crypto.cpp.

#pragma once

#include <stdint.h>
#include <vector>

namespace wa {
namespace crypto {

using Bytes = std::vector<uint8_t>;

// ── AES block cipher (encryption direction; CTR/GCM/CBC-encrypt need it) ──

class Aes256 {
public:
    explicit Aes256(const Bytes &key);          // requires 32-byte key
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint8_t roundKeys_[15 * 16];
};

// ── AES-256-GCM ────────────────────────────────────────────────────────────

// Returns ciphertext || 16-byte tag (Baileys' aesEncryptGCM layout).
Bytes aes256GcmEncrypt(const Bytes &plaintext, const Bytes &key,
                       const Bytes &iv, const Bytes &aad);

// Accepts ciphertext || 16-byte tag. Returns false when authentication fails.
bool aes256GcmDecrypt(const Bytes &ciphertextAndTag, const Bytes &key,
                      const Bytes &iv, const Bytes &aad, Bytes &plaintext);

// ── AES-256-CBC (media) ────────────────────────────────────────────────────

// PKCS#7-padded encryption.
Bytes aes256CbcEncrypt(const Bytes &plaintext, const Bytes &key, const Bytes &iv);
// Returns false on bad padding.
bool aes256CbcDecrypt(const Bytes &ciphertext, const Bytes &key,
                      const Bytes &iv, Bytes &plaintext);

// Same, but without stripping/validating PKCS#7 padding. WhatsApp's media
// format appends a padding region whose contents are random, so callers that
// know the real length (or strip the last byte themselves) need this.
bool aes256CbcDecryptRaw(const Bytes &ciphertext, const Bytes &key,
                         const Bytes &iv, Bytes &plaintext);

// AES-256-CTR (used by some media helpers).
Bytes aes256Ctr(const Bytes &input, const Bytes &key, const Bytes &iv);

} // namespace crypto
} // namespace wa
