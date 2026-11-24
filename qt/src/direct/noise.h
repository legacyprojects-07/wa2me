// WhatsApp Noise transport — faithful C++ port of Baileys
// lib/Utils/noise-handler.js (Noise_XX_25519_AESGCM_SHA256).
//
// Responsibilities:
//   * derive the handshake hash / salt / cipher keys
//   * process the server's ServerHello (Noise XX) and produce the client's
//     encrypted static key (`keyEnc`) for the ClientFinish message
//   * encrypt/decrypt every frame after the handshake
//   * frame the byte stream with WhatsApp's 3-byte big-endian length prefix
//     and the "WA\x06\x03" header on the first outgoing frame
//
// Crypto primitives come from the dependency-free implementations in this
// directory (sha256/hkdf, x25519, aes_gcm), so the whole transport is
// portable to Symbian. Certificate-chain validation is surfaced as a flag
// rather than enforced, because decoding CertChain fully needs WAProto.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "proto_min.h"
#include "sha256.h"

namespace wa {
namespace noise {

using Bytes = std::vector<uint8_t>;

// Defaults from Baileys Defaults/index.js
extern const char *const NOISE_MODE;               // "Noise_XX_25519_AESGCM_SHA256\0\0\0\0"
extern const uint8_t NOISE_HEADER[4];              // 87 65 6 3  ("WA\x06\x03")

class NoiseHandler {
public:
    // `ephPriv`/`ephPub`: the per-connection ephemeral X25519 key pair sent in
    // ClientHello (Baileys: `ephemeralKeyPair`). `noisePriv`/`noisePub` (the
    // long-lived creds `noiseKey`) are supplied to processHandshake, exactly
    // as Baileys passes `creds.noiseKey` there.
    NoiseHandler(Bytes ephPriv, Bytes ephPub, Bytes routingInfo = Bytes());

    // Noise XX step 2: consumes the server's ServerHello.
    // Returns the encrypted static key for the ClientFinish message.
    // Fills `certChainSerial` if the cert chain could be parsed.
    Bytes processHandshake(const proto::ServerHello &serverHello,
                           const Bytes &noisePriv, const Bytes &noisePub,
                           uint64_t *certChainSerial = nullptr,
                           bool *certValid = nullptr);

    // Encrypt/decrypt a post-handshake frame payload.
    Bytes encrypt(const Bytes &plaintext);
    bool decrypt(const Bytes &ciphertext, Bytes &plaintext);

    // Called after the ClientFinish message has been sent.
    void finishInit();

    // Handshake-hash absorbing (exposed for tests / debugging).
    void authenticate(const Bytes &data);

    // Frame assembly / disassembly.
    Bytes encodeFrame(const Bytes &data);          // adds header/length prefix
    // Feeds raw websocket bytes; invokes onFrame for each complete payload.
    void decodeFrames(const Bytes &newData,
                      const std::function<void(const Bytes &)> &onFrame);

    bool isFinished() const { return finished_; }
    const Bytes &hash() const { return hash_; }

private:
    void mixIntoKey(const Bytes &data);
    void localHKDF(const Bytes &data, Bytes &writeKey, Bytes &readKey);
    static Bytes generateIV(uint32_t counter);

    Bytes ephPriv_;
    Bytes ephPub_;
    Bytes routingInfo_;

    Bytes hash_;
    Bytes salt_;
    Bytes encKey_;
    Bytes decKey_;

    uint32_t readCounter_ = 0;
    uint32_t writeCounter_ = 0;
    bool finished_ = false;
    bool sentIntro_ = false;

    Bytes inBytes_;
};

} // namespace noise
} // namespace wa
