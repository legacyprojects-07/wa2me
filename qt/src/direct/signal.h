// Signal Protocol (X3DH + Double Ratchet) for WhatsApp — dependency-free C++17.
//
// A faithful port of libsignal's session_builder.js / session_cipher.js and
// the WhisperTextProtocol messages, which is what Baileys uses under the name
// `@whiskeysockets/libsignal-node`. WhatsApp's variant differs from vanilla
// libsignal only in the 0x05 "key bundle type" prefix on public keys and the
// protobuf names (WAProto's <enc> payloads carry the same bytes).
//
//   * X3DH:  DH(identity, signedPreKey) || DH(base, identity)
//            || DH(base, signedPreKey) [|| DH(base, ephemeral)]
//            mixed with the 0xFF*32 "discontinuity" prefix
//   * Ratchet: rootKey/chainKey from HKDF-SHA256("WhisperRatchet"), message
//            keys from HMAC-SHA256(counter), AES-256-CBC + truncated HMAC
//   * Message: version byte 0x33 || WhisperMessage || 8-byte MAC
//
// Verified against libsignal itself in qt/tests/test_signal.cpp: sessions
// built by the JS library are decrypted here, and vice versa.

#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#include "proto_min.h"

namespace wa {
namespace signal {

using Bytes = std::vector<uint8_t>;

// Public keys carry WhatsApp's 0x05 prefix (33 bytes), private keys are 32.
struct KeyPair {
    Bytes priv;
    Bytes pub;    // 33 bytes (0x05 || x25519)
    bool valid() const { return priv.size() == 32 && pub.size() == 33; }
};

KeyPair generateKeyPair();
KeyPair keyPairFromPrivate(const Bytes &priv);
// Strips a leading 0x05 if present (libsignal's scrubPubKeyFormat).
Bytes rawPublicKey(const Bytes &pub);

struct PreKeyRecord {
    uint32_t id;
    KeyPair pair;
    Bytes signature;   // 64 bytes (XEdDSA) for signed pre-keys
    PreKeyRecord() : id(0) {}
    PreKeyRecord(uint32_t id_, const KeyPair &pair_, const Bytes &sig)
        : id(id_), pair(pair_), signature(sig) {}
};

// A remote device as returned by WhatsApp's pre-key bundle fetch.
struct Bundle {
    Bytes identityKey;             // 33 bytes
    uint32_t registrationId;
    PreKeyRecord signedPreKey;
    bool hasPreKey;
    PreKeyRecord preKey;
    Bundle() : registrationId(0), hasPreKey(false) {}
};

// ── Session state (mirrors libsignal's SessionRecord/Chain) ────────────────

enum class ChainType { Sending = 0, Receiving = 1 };

struct Chain {
    Bytes ephemeralKey;                 // chain id = ephemeral public key
    Bytes chainKey;                     // 32-byte seed
    int32_t counter;
    ChainType type;
    std::map<uint32_t, Bytes> messageKeys;
    bool closed;
    Chain() : counter(-1), type(ChainType::Sending), closed(false) {}
};

struct PendingPreKey {
    bool active;
    uint32_t signedKeyId;
    bool hasPreKeyId;
    uint32_t preKeyId;
    Bytes baseKey;                      // 33 bytes
    PendingPreKey() : active(false), signedKeyId(0), hasPreKeyId(false), preKeyId(0) {}
};

struct Session {
    // indexInfo
    Bytes remoteIdentityKey;            // 33 bytes
    Bytes baseKey;                      // 33 bytes
    bool baseKeyOurs;
    bool closed;
    uint64_t used;

    uint32_t registrationId;
    Bytes rootKey;                      // 32 bytes
    KeyPair ephemeralKeyPair;
    Bytes lastRemoteEphemeralKey;       // 33 bytes
    uint32_t previousCounter;
    std::vector<Chain> chains;
    PendingPreKey pendingPreKey;

    Chain *chain(const Bytes &ephemeralKey);
    Chain *addChain(Chain c);
    void deleteChain(const Bytes &ephemeralKey);
    Session()
        : baseKeyOurs(true),
          closed(false),
          used(0),
          registrationId(0),
          previousCounter(0) {}
};

struct Record {
    std::vector<Session> sessions;
    Session *openSession();             // first non-closed session
    Session *sessionForBaseKey(const Bytes &baseKey);
    void setSession(Session s);         // replaces same baseKey, closes others
};

// ── Store ─────────────────────────────────────────────────────────────────

// Persistence hook. The Qt layer backs this with a JSON file; tests use an
// in-memory implementation.
class Store {
public:
    virtual ~Store() = default;

    virtual KeyPair ourIdentity() = 0;
    virtual uint32_t ourRegistrationId() = 0;
    virtual bool loadPreKey(uint32_t id, KeyPair &out) = 0;
    virtual void removePreKey(uint32_t id) = 0;
    virtual bool loadSignedPreKey(uint32_t id, KeyPair &out) = 0;

    virtual Record loadSession(const std::string &address) = 0;
    virtual void storeSession(const std::string &address, const Record &record) = 0;
};

// ── Session establishment and messaging ───────────────────────────────────

// Builds an outgoing session from a remote bundle (libsignal initOutgoing).
bool initOutgoingSession(Store &store, const std::string &address,
                         const Bundle &bundle, std::string &error);

// Result of encrypting: type 3 = PreKeyWhisperMessage, 1 = WhisperMessage.
struct EncryptResult {
    int type;
    Bytes body;
    uint32_t registrationId;
    bool ok;
    std::string error;
    EncryptResult() : type(1), registrationId(0), ok(false) {}
};

EncryptResult encrypt(Store &store, const std::string &address, const Bytes &plaintext);

// Decrypts either a WhisperMessage (type 1) or PreKeyWhisperMessage (type 3).
bool decrypt(Store &store, const std::string &address, int type, const Bytes &body,
             Bytes &plaintext, std::string &error);

// ── Serialisation (used for on-disk session storage) ──────────────────────
Bytes serializeRecord(const Record &record);
bool deserializeRecord(const Bytes &data, Record &out);

Bytes serializeBundle(const Bundle &bundle);
bool deserializeBundle(const Bytes &data, Bundle &out);

} // namespace signal
} // namespace wa
