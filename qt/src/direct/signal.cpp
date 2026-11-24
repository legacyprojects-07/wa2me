#include "signal.h"

#include "compat.h"

#include <cstring>
#include <stdexcept>

#include "aes_gcm.h"
#include "sha256.h"
#include "x25519.h"

namespace wa {
namespace signal {

namespace {

constexpr uint8_t kKeyBundleType = 0x05;
constexpr int kVersion = 3;

// ── Small helpers ─────────────────────────────────────────────────────────

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    size_t total = 0;
    for (const auto &p : parts) total += p.size();
    out.reserve(total);
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// libsignal's deriveSecrets: HKDF-SHA256(input, salt, info) -> 3 x 32 bytes
void deriveSecrets(const Bytes &input, const Bytes &salt, const std::string &info,
                   Bytes &cipherKey, Bytes &macKey, Bytes &iv) {
    const Bytes out = crypto::hkdfSha256(input, 96, salt,
                                         Bytes(info.begin(), info.end()));
    cipherKey.assign(out.begin(), out.begin() + 32);
    macKey.assign(out.begin() + 32, out.begin() + 64);
    iv.assign(out.begin() + 64, out.begin() + 96);
}

Bytes mac(const Bytes &key, const Bytes &data) { return crypto::hmacSha256(key, data); }

uint8_t tupleByte(int a, int b) { return uint8_t(((a & 0xf) << 4) | (b & 0xf)); }

// Curve25519 agreement with 0x05-prefixed public keys.
bool agreement(const Bytes &pub, const Bytes &priv, Bytes &out) {
    const Bytes raw = rawPublicKey(pub);
    if (raw.size() != 32 || priv.size() != 32) return false;
    out.resize(32);
    return crypto::x25519(priv.data(), raw.data(), out.data());
}

// ── WhisperTextProtocol messages ──────────────────────────────────────────

struct WhisperMessage {
    Bytes ephemeralKey;
    uint32_t counter = 0;
    uint32_t previousCounter = 0;
    Bytes ciphertext;
};

Bytes encodeWhisperMessage(const WhisperMessage &m) {
    proto::Writer w;
    if (!m.ephemeralKey.empty()) w.fieldBytes(1, m.ephemeralKey);
    w.fieldVarint(2, m.counter);
    w.fieldVarint(3, m.previousCounter);
    if (!m.ciphertext.empty()) w.fieldBytes(4, m.ciphertext);
    return w.data();
}

bool decodeWhisperMessage(const Bytes &data, WhisperMessage &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.ephemeralKey = f.bytes; break;
        case 2: out.counter = uint32_t(f.varint); break;
        case 3: out.previousCounter = uint32_t(f.varint); break;
        case 4: out.ciphertext = f.bytes; break;
        default: break;
        }
    }
    return !out.ephemeralKey.empty() && !out.ciphertext.empty();
}

struct PreKeyWhisperMessage {
    bool hasPreKeyId = false;
    uint32_t preKeyId = 0;
    Bytes baseKey;
    Bytes identityKey;
    Bytes message;
    uint32_t registrationId = 0;
    uint32_t signedPreKeyId = 0;
};

Bytes encodePreKeyWhisperMessage(const PreKeyWhisperMessage &m) {
    proto::Writer w;
    if (m.hasPreKeyId) w.fieldVarint(1, m.preKeyId);
    if (!m.baseKey.empty()) w.fieldBytes(2, m.baseKey);
    if (!m.identityKey.empty()) w.fieldBytes(3, m.identityKey);
    if (!m.message.empty()) w.fieldBytes(4, m.message);
    w.fieldVarint(5, m.registrationId);
    w.fieldVarint(6, m.signedPreKeyId);
    return w.data();
}

bool decodePreKeyWhisperMessage(const Bytes &data, PreKeyWhisperMessage &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.hasPreKeyId = true; out.preKeyId = uint32_t(f.varint); break;
        case 2: out.baseKey = f.bytes; break;
        case 3: out.identityKey = f.bytes; break;
        case 4: out.message = f.bytes; break;
        case 5: out.registrationId = uint32_t(f.varint); break;
        case 6: out.signedPreKeyId = uint32_t(f.varint); break;
        default: break;
        }
    }
    return !out.baseKey.empty() && !out.identityKey.empty() && !out.message.empty();
}

// ── Ratchet ───────────────────────────────────────────────────────────────

// Counters are signed (they start at -1), exactly like libsignal's JS numbers.
void fillMessageKeys(Chain &chain, int32_t counter) {
    if (chain.counter >= counter) return;
    if (counter - chain.counter > 2000)
        throw std::runtime_error("Over 2000 messages into the future");
    if (chain.chainKey.empty() || chain.closed)
        throw std::runtime_error("Chain closed");

    while (chain.counter < counter) {
        chain.messageKeys[uint32_t(chain.counter + 1)] = mac(chain.chainKey, Bytes{0x01});
        chain.chainKey = mac(chain.chainKey, Bytes{0x02});
        chain.counter += 1;
    }
}

void calculateRatchet(Session &session, const Bytes &remoteKey, bool sending) {
    Bytes shared;
    if (!agreement(remoteKey, session.ephemeralKeyPair.priv, shared))
        throw std::runtime_error("ratchet agreement failed");

    Bytes cipherKey, macKey, iv;
    deriveSecrets(shared, session.rootKey, "WhisperRatchet", cipherKey, macKey, iv);

    Chain chain;
    chain.ephemeralKey = sending ? session.ephemeralKeyPair.pub : remoteKey;
    chain.chainKey = macKey;      // masterKey[1]
    chain.counter = -1;
    chain.type = sending ? ChainType::Sending : ChainType::Receiving;
    session.addChain(chain);

    session.rootKey = cipherKey;  // masterKey[0]
}

void maybeStepRatchet(Session &session, const Bytes &remoteKey, uint32_t previousCounter) {
    if (session.chain(remoteKey)) return;

    Chain *previous = session.chain(session.lastRemoteEphemeralKey);
    if (previous) {
        fillMessageKeys(*previous, int32_t(previousCounter));
        previous->chainKey.clear();   // close
        previous->closed = true;
    }

    calculateRatchet(session, remoteKey, false);

    Chain *ourChain = session.chain(session.ephemeralKeyPair.pub);
    if (ourChain) {
        session.previousCounter = uint32_t(ourChain->counter);
        session.deleteChain(session.ephemeralKeyPair.pub);
    }

    session.ephemeralKeyPair = generateKeyPair();
    calculateRatchet(session, remoteKey, true);
    session.lastRemoteEphemeralKey = remoteKey;
}

// ── X3DH ──────────────────────────────────────────────────────────────────

Session initSession(bool initiator, const KeyPair &ourEphemeral, const KeyPair *ourSignedKey,
                    const Bytes &theirIdentity, const Bytes *theirEphemeral,
                    const Bytes *theirSigned, uint32_t registrationId,
                    const KeyPair &ourIdentity) {
    KeyPair signedKey = ourSignedKey ? *ourSignedKey : ourEphemeral;
    Bytes theirSignedKey = theirSigned ? *theirSigned : *theirEphemeral;

    Bytes sharedSecret(32, 0xff);
    const size_t total = (ourEphemeral.valid() && theirEphemeral) ? 32 * 5 : 32 * 4;
    sharedSecret.resize(total, 0);

    Bytes a1, a2, a3;
    if (!agreement(theirSignedKey, ourIdentity.priv, a1)) throw std::runtime_error("x3dh a1");
    if (!agreement(theirIdentity, signedKey.priv, a2)) throw std::runtime_error("x3dh a2");
    if (!agreement(theirSignedKey, signedKey.priv, a3)) throw std::runtime_error("x3dh a3");

    // Layout: 0xFF*32 || DH1 || DH2 || DH3 [|| DH4]
    if (initiator) {
        std::memcpy(sharedSecret.data() + 32, a1.data(), 32);
        std::memcpy(sharedSecret.data() + 64, a2.data(), 32);
    } else {
        std::memcpy(sharedSecret.data() + 32, a2.data(), 32);
        std::memcpy(sharedSecret.data() + 64, a1.data(), 32);
    }
    std::memcpy(sharedSecret.data() + 96, a3.data(), 32);

    if (ourEphemeral.valid() && theirEphemeral) {
        Bytes a4;
        if (!agreement(*theirEphemeral, ourEphemeral.priv, a4)) throw std::runtime_error("x3dh a4");
        std::memcpy(sharedSecret.data() + 128, a4.data(), 32);
    }

    Bytes masterKey = crypto::hkdfSha256(sharedSecret, 64, Bytes(32, 0),
                                         Bytes{'W','h','i','s','p','e','r','T','e','x','t'});

    Session session;
    session.registrationId = registrationId;
    session.rootKey.assign(masterKey.begin(), masterKey.begin() + 32);
    session.ephemeralKeyPair = initiator ? generateKeyPair() : signedKey;
    session.lastRemoteEphemeralKey = theirSignedKey;
    session.previousCounter = 0;
    session.remoteIdentityKey = theirIdentity;
    session.baseKey = initiator ? ourEphemeral.pub : *theirEphemeral;
    session.baseKeyOurs = initiator;

    if (initiator) {
        Bytes shared;
        if (!agreement(theirSignedKey, session.ephemeralKeyPair.priv, shared))
            throw std::runtime_error("sending ratchet agreement failed");
        Bytes cipherKey, macKey, iv;
        deriveSecrets(shared, session.rootKey, "WhisperRatchet", cipherKey, macKey, iv);
        Chain chain;
        chain.ephemeralKey = session.ephemeralKeyPair.pub;
        chain.chainKey = macKey;
        chain.counter = -1;
        chain.type = ChainType::Sending;
        session.addChain(chain);
        session.rootKey = cipherKey;
    }
    // masterKey[1] is unused here (libsignal keeps only [0] for rootKey).
    return session;
}

} // namespace

// ── Key helpers ───────────────────────────────────────────────────────────

Bytes rawPublicKey(const Bytes &pub) {
    if (pub.size() == 33 && pub[0] == kKeyBundleType) return Bytes(pub.begin() + 1, pub.end());
    return pub;
}

KeyPair keyPairFromPrivate(const Bytes &priv) {
    KeyPair kp;
    if (priv.size() != 32) return kp;
    kp.priv = priv;
    kp.pub.resize(33);
    kp.pub[0] = kKeyBundleType;
    if (!crypto::x25519Base(priv.data(), kp.pub.data() + 1)) return KeyPair();
    return kp;
}

KeyPair generateKeyPair() {
    const crypto::CurveKeyPair kp = crypto::generateCurveKeyPair();
    return keyPairFromPrivate(kp.priv);
}

// ── Record / chain helpers ────────────────────────────────────────────────

Chain *Session::chain(const Bytes &ephemeralKey) {
    for (auto &c : chains) {
        if (c.ephemeralKey == ephemeralKey) return &c;
    }
    return nullptr;
}

Chain *Session::addChain(Chain c) {
    if (Chain *existing = chain(c.ephemeralKey)) {
        *existing = std::move(c);
        return existing;
    }
    chains.push_back(std::move(c));
    return &chains.back();
}

void Session::deleteChain(const Bytes &ephemeralKey) {
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        if (it->ephemeralKey == ephemeralKey) {
            chains.erase(it);
            return;
        }
    }
}

Session *Record::openSession() {
    for (auto &s : sessions) {
        if (!s.closed) return &s;
    }
    return nullptr;
}

Session *Record::sessionForBaseKey(const Bytes &baseKey) {
    for (auto &s : sessions) {
        if (s.baseKey == baseKey) return &s;
    }
    return nullptr;
}

void Record::setSession(Session s) {
    for (auto &existing : sessions) {
        if (existing.baseKey == s.baseKey) {
            existing = std::move(s);
            return;
        }
    }
    sessions.push_back(std::move(s));
}

// ── Public API ────────────────────────────────────────────────────────────

bool initOutgoingSession(Store &store, const std::string &address,
                         const Bundle &bundle, std::string &error) {
    try {
        if (bundle.identityKey.size() != 33) {
            error = "invalid identity key";
            return false;
        }
        // Remote bundles only need public keys (private halves stay remote).
        if (rawPublicKey(bundle.signedPreKey.pair.pub).size() != 32) {
            error = "invalid signed pre-key";
            return false;
        }
        if (bundle.hasPreKey && rawPublicKey(bundle.preKey.pair.pub).size() != 32) {
            error = "invalid pre-key";
            return false;
        }
        // libsignal skips signature verification for outgoing sessions
        // (verifySignature(..., isInit=true)), so we do the same.

        const KeyPair baseKey = generateKeyPair();
        const KeyPair ourIdentity = store.ourIdentity();

        Session session = initSession(true, baseKey, nullptr, bundle.identityKey,
                                      bundle.hasPreKey ? &bundle.preKey.pair.pub : nullptr,
                                      &bundle.signedPreKey.pair.pub,
                                      bundle.registrationId, ourIdentity);
        session.pendingPreKey.active = true;
        session.pendingPreKey.signedKeyId = bundle.signedPreKey.id;
        session.pendingPreKey.baseKey = baseKey.pub;
        if (bundle.hasPreKey) {
            session.pendingPreKey.hasPreKeyId = true;
            session.pendingPreKey.preKeyId = bundle.preKey.id;
        }

        Record record = store.loadSession(address);
        for (auto &existing : record.sessions) existing.closed = true;
        record.setSession(std::move(session));
        store.storeSession(address, record);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

EncryptResult encrypt(Store &store, const std::string &address, const Bytes &plaintext) {
    EncryptResult result;
    try {
        Record record = store.loadSession(address);
        Session *session = record.openSession();
        if (!session) {
            result.error = "No sessions";
            return result;
        }
        Chain *chain = session->chain(session->ephemeralKeyPair.pub);
        if (!chain) {
            result.error = "No sending chain";
            return result;
        }
        if (chain->type == ChainType::Receiving) {
            result.error = "Tried to encrypt on a receiving chain";
            return result;
        }

        fillMessageKeys(*chain, chain->counter + 1);
        const uint32_t counter = uint32_t(chain->counter);
        auto it = chain->messageKeys.find(counter);
        if (it == chain->messageKeys.end()) {
            result.error = "No message key";
            return result;
        }
        Bytes messageKey = it->second;
        chain->messageKeys.erase(it);

        Bytes cipherKey, macKey, iv;
        deriveSecrets(messageKey, Bytes(32, 0), "WhisperMessageKeys", cipherKey, macKey, iv);

        WhisperMessage msg;
        msg.ephemeralKey = session->ephemeralKeyPair.pub;
        msg.counter = counter;
        msg.previousCounter = session->previousCounter;
        msg.ciphertext = crypto::aes256CbcEncrypt(plaintext, cipherKey,
                                                  Bytes(iv.begin(), iv.begin() + 16));

        const Bytes msgBuf = encodeWhisperMessage(msg);

        const KeyPair ourIdentity = store.ourIdentity();
        Bytes macInput = concat({ourIdentity.pub, session->remoteIdentityKey,
                                 Bytes{tupleByte(kVersion, kVersion)}, msgBuf});
        const Bytes full = mac(macKey, macInput);

        Bytes framed;
        framed.push_back(tupleByte(kVersion, kVersion));
        framed.insert(framed.end(), msgBuf.begin(), msgBuf.end());
        framed.insert(framed.end(), full.begin(), full.begin() + 8);

        if (session->pendingPreKey.active) {
            PreKeyWhisperMessage pre;
            pre.identityKey = ourIdentity.pub;
            pre.registrationId = store.ourRegistrationId();
            pre.baseKey = session->pendingPreKey.baseKey;
            pre.signedPreKeyId = session->pendingPreKey.signedKeyId;
            pre.message = framed;
            if (session->pendingPreKey.hasPreKeyId) {
                pre.hasPreKeyId = true;
                pre.preKeyId = session->pendingPreKey.preKeyId;
            }
            const Bytes encoded = encodePreKeyWhisperMessage(pre);
            result.type = 3;
            result.body.push_back(tupleByte(kVersion, kVersion));
            result.body.insert(result.body.end(), encoded.begin(), encoded.end());
            // NB: libsignal keeps the pending pre-key *active* until the peer
            // answers (SessionCipher.clearUnacknowledgedPreKeyMessage on
            // decrypt). Every message sent before the first reply is therefore
            // a PreKeyWhisperMessage, so a lost first message cannot strand
            // the recipient without a session.
        } else {
            result.type = 1;
            result.body = framed;
        }

        result.registrationId = session->registrationId;
        record.setSession(*session);
        store.storeSession(address, record);
        result.ok = true;
        return result;
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    }
}

namespace {

bool decryptWhisper(Session &session, const Bytes &buffer, const Bytes &ourIdentityPub,
                    Bytes &plaintext, std::string &error) {
    if (buffer.size() < 1 + 8) {
        error = "message too short";
        return false;
    }
    const uint8_t versions = buffer[0];
    if ((versions & 0xf) > kVersion || (versions >> 4) < kVersion) {
        error = "incompatible version";
        return false;
    }

    const Bytes messageProto(buffer.begin() + 1, buffer.end() - 8);
    const Bytes theirMac(buffer.end() - 8, buffer.end());

    WhisperMessage message;
    if (!decodeWhisperMessage(messageProto, message)) {
        error = "bad WhisperMessage";
        return false;
    }

    maybeStepRatchet(session, message.ephemeralKey, message.previousCounter);

    Chain *chain = session.chain(message.ephemeralKey);
    if (!chain) {
        error = "no chain for ephemeral key";
        return false;
    }
    if (chain->type == ChainType::Sending) {
        error = "tried to decrypt on a sending chain";
        return false;
    }

    fillMessageKeys(*chain, int32_t(message.counter));
    auto it = chain->messageKeys.find(message.counter);
    if (it == chain->messageKeys.end()) {
        error = "key used already or never filled";
        return false;
    }
    Bytes messageKey = it->second;
    chain->messageKeys.erase(it);

    Bytes cipherKey, macKey, iv;
    deriveSecrets(messageKey, Bytes(32, 0), "WhisperMessageKeys", cipherKey, macKey, iv);

    Bytes macInput = concat({session.remoteIdentityKey, ourIdentityPub,
                             Bytes{tupleByte(kVersion, kVersion)}, messageProto});
    const Bytes full = mac(macKey, macInput);
    if (!crypto::constantTimeEquals(Bytes(full.begin(), full.begin() + 8), theirMac)) {
        error = "bad MAC";
        return false;
    }

    if (!crypto::aes256CbcDecrypt(message.ciphertext, cipherKey,
                                  Bytes(iv.begin(), iv.begin() + 16), plaintext)) {
        error = "bad padding";
        return false;
    }
    session.pendingPreKey.active = false;
    return true;
}

} // namespace

bool decrypt(Store &store, const std::string &address, int type, const Bytes &body,
             Bytes &plaintext, std::string &error) {
    try {
        const KeyPair ourIdentity = store.ourIdentity();
        Record record = store.loadSession(address);

        if (type == 1) {
            bool ok = false;
            for (auto &session : record.sessions) {
                Bytes attempt;
                std::string err;
                if (decryptWhisper(session, body, ourIdentity.pub, attempt, err)) {
                    plaintext = attempt;
                    ok = true;
                    break;
                }
                error = err;
            }
            if (!ok) return false;
            store.storeSession(address, record);
            return true;
        }

        if (type != 3) {
            error = "unsupported ciphertext type " + compat::toString(type);
            return false;
        }

        // PreKeyWhisperMessage: build the inbound session first.
        if (body.empty()) {
            error = "empty pre-key message";
            return false;
        }
        const uint8_t versions = body[0];
        if ((versions & 0xf) > kVersion || (versions >> 4) < kVersion) {
            error = "incompatible version on PreKeyWhisperMessage";
            return false;
        }

        PreKeyWhisperMessage pre;
        if (!decodePreKeyWhisperMessage(Bytes(body.begin() + 1, body.end()), pre)) {
            error = "bad PreKeyWhisperMessage";
            return false;
        }
        if (record.sessionForBaseKey(pre.baseKey)) {
            // Already have the session: just decrypt the inner message.
            Session *session = record.sessionForBaseKey(pre.baseKey);
            if (!decryptWhisper(*session, pre.message, ourIdentity.pub, plaintext, error))
                return false;
            store.storeSession(address, record);
            return true;
        }

        KeyPair preKeyPair;
        bool havePreKey = false;
        if (pre.hasPreKeyId) {
            havePreKey = store.loadPreKey(pre.preKeyId, preKeyPair);
            // libsignal treats an unknown/consumed one-time pre-key as "not
            // present" and falls back to the signed pre-key alone (no DH4).
            // If the sender really used that pre-key the MAC check will fail,
            // which is the correct outcome; failing earlier would also drop
            // messages that legitimately used the signed pre-key only.
            if (!havePreKey) preKeyPair = KeyPair();
        }
        KeyPair signedPreKeyPair;
        if (!store.loadSignedPreKey(pre.signedPreKeyId, signedPreKeyPair)) {
            error = "missing signed pre-key " + compat::toString(pre.signedPreKeyId);
            return false;
        }

        for (auto &existing : record.sessions) existing.closed = true;

        Session session = initSession(false, preKeyPair, &signedPreKeyPair, pre.identityKey,
                                      &pre.baseKey, nullptr, pre.registrationId, ourIdentity);
        record.setSession(std::move(session));

        Session *created = record.sessionForBaseKey(pre.baseKey);
        if (!created || !decryptWhisper(*created, pre.message, ourIdentity.pub, plaintext, error))
            return false;

        store.storeSession(address, record);
        if (havePreKey) store.removePreKey(pre.preKeyId);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

// ── Serialisation ─────────────────────────────────────────────────────────

namespace {

void put(Bytes &out, const Bytes &v) {
    out.push_back(uint8_t(v.size() >> 24));
    out.push_back(uint8_t(v.size() >> 16));
    out.push_back(uint8_t(v.size() >> 8));
    out.push_back(uint8_t(v.size()));
    out.insert(out.end(), v.begin(), v.end());
}

void putU32(Bytes &out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

struct Cursor {
    explicit Cursor(const Bytes &bytes) : data(bytes), pos(0), ok(true) {}

    const Bytes &data;
    size_t pos;
    bool ok;

    bool take(Bytes &out, size_t n) {
        if (pos + n > data.size()) { ok = false; return false; }
        out.assign(data.begin() + pos, data.begin() + pos + n);
        pos += n;
        return true;
    }
    Bytes blob() {
        Bytes len;
        if (!take(len, 4)) return Bytes();
        const uint32_t n = (uint32_t(len[0]) << 24) | (uint32_t(len[1]) << 16) |
                           (uint32_t(len[2]) << 8) | len[3];
        Bytes out;
        take(out, n);
        return out;
    }
    uint32_t u32() {
        Bytes b;
        if (!take(b, 4)) return 0;
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    }
};

} // namespace

Bytes serializeRecord(const Record &record) {
    Bytes out;
    putU32(out, uint32_t(record.sessions.size()));
    for (const auto &s : record.sessions) {
        put(out, s.remoteIdentityKey);
        put(out, s.baseKey);
        out.push_back(s.baseKeyOurs ? 1 : 0);
        out.push_back(s.closed ? 1 : 0);
        putU32(out, s.registrationId);
        put(out, s.rootKey);
        put(out, s.ephemeralKeyPair.priv);
        put(out, s.ephemeralKeyPair.pub);
        put(out, s.lastRemoteEphemeralKey);
        putU32(out, s.previousCounter);
        out.push_back(s.pendingPreKey.active ? 1 : 0);
        putU32(out, s.pendingPreKey.signedKeyId);
        out.push_back(s.pendingPreKey.hasPreKeyId ? 1 : 0);
        putU32(out, s.pendingPreKey.preKeyId);
        put(out, s.pendingPreKey.baseKey);
        putU32(out, uint32_t(s.chains.size()));
        for (const auto &c : s.chains) {
            put(out, c.ephemeralKey);
            put(out, c.chainKey);
            putU32(out, uint32_t(c.counter));
            out.push_back(c.type == ChainType::Sending ? 0 : 1);
            out.push_back(c.closed ? 1 : 0);
            putU32(out, uint32_t(c.messageKeys.size()));
            for (const auto &mk : c.messageKeys) {
                putU32(out, mk.first);
                put(out, mk.second);
            }
        }
    }
    return out;
}

bool deserializeRecord(const Bytes &data, Record &out) {
    Cursor cur(data);
    out.sessions.clear();
    const uint32_t count = cur.u32();
    for (uint32_t i = 0; i < count && cur.ok; i++) {
        Session s;
        s.remoteIdentityKey = cur.blob();
        s.baseKey = cur.blob();
        Bytes flag;
        if (!cur.take(flag, 1)) return false;
        s.baseKeyOurs = flag[0] != 0;
        if (!cur.take(flag, 1)) return false;
        s.closed = flag[0] != 0;
        s.registrationId = cur.u32();
        s.rootKey = cur.blob();
        s.ephemeralKeyPair.priv = cur.blob();
        s.ephemeralKeyPair.pub = cur.blob();
        s.lastRemoteEphemeralKey = cur.blob();
        s.previousCounter = cur.u32();
        if (!cur.take(flag, 1)) return false;
        s.pendingPreKey.active = flag[0] != 0;
        s.pendingPreKey.signedKeyId = cur.u32();
        if (!cur.take(flag, 1)) return false;
        s.pendingPreKey.hasPreKeyId = flag[0] != 0;
        s.pendingPreKey.preKeyId = cur.u32();
        s.pendingPreKey.baseKey = cur.blob();
        const uint32_t chainCount = cur.u32();
        for (uint32_t c = 0; c < chainCount && cur.ok; c++) {
            Chain chain;
            chain.ephemeralKey = cur.blob();
            chain.chainKey = cur.blob();
            chain.counter = int32_t(cur.u32());
            if (!cur.take(flag, 1)) return false;
            chain.type = flag[0] == 0 ? ChainType::Sending : ChainType::Receiving;
            if (!cur.take(flag, 1)) return false;
            chain.closed = flag[0] != 0;
            const uint32_t mkCount = cur.u32();
            for (uint32_t m = 0; m < mkCount && cur.ok; m++) {
                const uint32_t id = cur.u32();
                chain.messageKeys[id] = cur.blob();
            }
            s.chains.push_back(std::move(chain));
        }
        out.sessions.push_back(std::move(s));
    }
    return cur.ok;
}

Bytes serializeBundle(const Bundle &b) {
    Bytes out;
    put(out, b.identityKey);
    putU32(out, b.registrationId);
    putU32(out, b.signedPreKey.id);
    put(out, b.signedPreKey.pair.priv);
    put(out, b.signedPreKey.pair.pub);
    put(out, b.signedPreKey.signature);
    out.push_back(b.hasPreKey ? 1 : 0);
    putU32(out, b.preKey.id);
    put(out, b.preKey.pair.priv);
    put(out, b.preKey.pair.pub);
    return out;
}

bool deserializeBundle(const Bytes &data, Bundle &out) {
    Cursor cur(data);
    out.identityKey = cur.blob();
    out.registrationId = cur.u32();
    out.signedPreKey.id = cur.u32();
    out.signedPreKey.pair.priv = cur.blob();
    out.signedPreKey.pair.pub = cur.blob();
    out.signedPreKey.signature = cur.blob();
    Bytes flag;
    if (!cur.take(flag, 1)) return false;
    out.hasPreKey = flag[0] != 0;
    out.preKey.id = cur.u32();
    out.preKey.pair.priv = cur.blob();
    out.preKey.pair.pub = cur.blob();
    return cur.ok;
}

} // namespace signal
} // namespace wa
