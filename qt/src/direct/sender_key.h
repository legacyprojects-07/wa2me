// Signal *sender keys* — group messaging ("skmsg") for WhatsApp.
//
// Port of Baileys' lib/Signal/Group/* (which is itself a port of
// libsignal-protocol's group layer). It differs from the 1:1 ratchet in that
// each sender keeps one chain per (group, sender) pair and distributes the
// chain head to the other members in a SenderKeyDistributionMessage:
//
//   chainKey_next = HMAC-SHA256(chainKey, [0x02])
//   messageSeed   = HMAC-SHA256(chainKey, [0x01])
//   keys          = HKDF-SHA256(messageSeed, 64, salt = 32 zero bytes,
//                               info = "WhisperGroup")
//   iv            = keys[0..16]
//   cipherKey     = keys[16..32] || keys[32..48]
//   message       = 0x33 || proto{1 id, 2 iteration, 3 ciphertext} || sig(64)
//   signature     = XEdDSA(signingKey, 0x33 || proto)
//
// The distribution message travels inside a normal 1:1 encrypted
// Message.senderKeyDistributionMessage (WAProto, field 2), so a client must
// have a Signal session with every member device anyway.
//
// Verified against Baileys' GroupCipher in qt/tests/test_sender_key.cpp.

#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#include "signal.h"

namespace wa {
namespace signal {

// One sender's chain for one group. `signingKey` has a private half only for
// our own state; for received distributions only the public key is known.
struct SenderKeyState {
    uint32_t keyId;
    Bytes chainKey;                    // 32 bytes
    uint32_t iteration;
    KeyPair signingKey;                // priv 32 / pub 33 (0x05-prefixed)
    std::map<uint32_t, Bytes> skipped; // iteration -> message seed
    SenderKeyState() : keyId(0), iteration(0) {}
};

// Signal's SenderKeyDistributionMessage payload (also the WAProto layout of
// SignalSenderKeyDistributionMessage: 1 id, 2 iteration, 3 chainKey,
// 4 signingKey).
struct SenderKeyDistribution {
    uint32_t id;
    uint32_t iteration;
    Bytes chainKey;        // 32
    Bytes signingKey;      // 33 (0x05-prefixed)
    SenderKeyDistribution() : id(0), iteration(0) {}
};

// 0x33 || proto{...} — the bytes WhatsApp carries in
// Message.senderKeyDistributionMessage.axolotlSenderKeyDistributionMessage.
Bytes encodeDistribution(const SenderKeyDistribution &dist);
bool decodeDistribution(const Bytes &data, SenderKeyDistribution &out, std::string &error);

struct SenderKeyMessage {
    uint32_t id;
    uint32_t iteration;
    Bytes ciphertext;
    Bytes signature;       // 64 (not sent by encodeMessage; appended by encode)
    SenderKeyMessage() : id(0), iteration(0) {}
};

// Serialises and signs with our signing private key.
Bytes encodeSenderKeyMessage(const SenderKeyMessage &msg, const Bytes &signingPrivateKey,
                             std::string &error);
// Splits a 0x33||proto||sig blob (no verification).
bool decodeSenderKeyMessage(const Bytes &data, SenderKeyMessage &out, std::string &error);
// Verifies the trailing signature over everything before it.
bool verifySenderKeyMessage(const SenderKeyMessage &msg, const Bytes &signedPart,
                            const Bytes &signingPublicKey, std::string &error);

// Persistence hook (the Qt client backs this with creds.json).
class SenderKeyStore {
public:
    virtual ~SenderKeyStore() = default;
    virtual bool loadSenderKey(const std::string &name, SenderKeyState &out) = 0;
    virtual void storeSenderKey(const std::string &name, const SenderKeyState &state) = 0;
};

// "<groupJid>::<senderUserJid>" — one chain per group and sender device.
std::string senderKeyName(const std::string &groupJid, const std::string &senderJid);

// Returns our chain for the group, creating it (and its signing key) on first use.
bool createSenderKey(SenderKeyStore &store, const std::string &groupJid,
                     const std::string &meJid, SenderKeyDistribution &out,
                     std::string &error);

// Installs a chain received from another member.
bool processDistribution(SenderKeyStore &store, const std::string &groupJid,
                         const std::string &senderJid,
                         const SenderKeyDistribution &dist, std::string &error);

// Encrypts a group message body with our chain (and advances it).
bool encryptGroup(SenderKeyStore &store, const std::string &groupJid,
                  const std::string &meJid, const Bytes &plaintext, Bytes &skmsg,
                  std::string &error);

// Decrypts another member's group message (caching skipped message keys).
bool decryptGroup(SenderKeyStore &store, const std::string &groupJid,
                  const std::string &senderJid, const Bytes &skmsg, Bytes &plaintext,
                  std::string &error);

// Text serialisation for creds.json.
std::string serializeSenderKeyState(const SenderKeyState &state);
bool deserializeSenderKeyState(const std::string &text, SenderKeyState &out);

} // namespace signal
} // namespace wa
