#include "sender_key.h"

#include "compat.h"

#include <cstring>
#include <sstream>

#include "aes_gcm.h"
#include "binary_node.h"
#include "ed25519.h"
#include "sha256.h"

namespace wa {
namespace signal {

namespace {

using crypto::Bytes;

constexpr uint8_t kVersion = 0x33;
constexpr uint32_t kMaxFutureMessages = 2000;
constexpr size_t kMaxSkippedKeys = 2000;

Bytes concat(const Bytes &a, const Bytes &b) {
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// HMAC(chainKey, [seed])
Bytes derive(const Bytes &chainKey, uint8_t seed) {
    return crypto::hmacSha256(chainKey, Bytes{seed});
}

// messageSeed -> (cipherKey, iv), exactly like Baileys' SenderMessageKey:
// deriveSecrets(seed, zeros32, "WhisperGroup") gives 64 bytes; the IV is the
// first 16 and the cipher key is bytes 16..48.
bool messageKeys(const Bytes &seed, Bytes &cipherKey, Bytes &iv) {
    const Bytes zeroSalt(32, 0);
    const std::string info = "WhisperGroup";
    const Bytes derived = crypto::hkdfSha256(seed, 64, zeroSalt,
                                             Bytes(info.begin(), info.end()));
    if (derived.size() != 64) return false;
    iv.assign(derived.begin(), derived.begin() + 16);
    cipherKey.clear();
    cipherKey.insert(cipherKey.end(), derived.begin() + 16, derived.begin() + 48);
    return true;
}

std::string hexOf(const Bytes &b) { return crypto::toHex(b); }

} // namespace

// ── Distribution message ──────────────────────────────────────────────────

Bytes encodeDistribution(const SenderKeyDistribution &dist) {
    proto::Writer w;
    w.fieldVarint(1, dist.id);
    w.fieldVarint(2, dist.iteration);
    if (!dist.chainKey.empty()) w.fieldBytes(3, dist.chainKey);
    if (!dist.signingKey.empty()) w.fieldBytes(4, dist.signingKey);
    Bytes out{kVersion};
    const Bytes body = w.data();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool decodeDistribution(const Bytes &data, SenderKeyDistribution &out, std::string &error) {
    out = SenderKeyDistribution();
    if (data.size() < 2) {
        error = "distribution message too short";
        return false;
    }
    size_t offset = 0;
    if (data[0] == kVersion) offset = 1;   // tolerate a missing version byte
    proto::Reader r(data.data() + offset, data.size() - offset);
    proto::Field f;
    while (r.next(f)) {
        if (f.number == 1) out.id = uint32_t(f.varint);
        else if (f.number == 2) out.iteration = uint32_t(f.varint);
        else if (f.number == 3) out.chainKey = f.bytes;
        else if (f.number == 4) out.signingKey = f.bytes;
    }
    if (out.chainKey.size() != 32) {
        error = "distribution message has no 32-byte chain key";
        return false;
    }
    if (out.signingKey.size() != 32 && out.signingKey.size() != 33) {
        error = "distribution message has no signing key";
        return false;
    }
    if (out.signingKey.size() == 32) out.signingKey.insert(out.signingKey.begin(), 0x05);
    return true;
}

// ── SenderKeyMessage ──────────────────────────────────────────────────────

Bytes encodeSenderKeyMessage(const SenderKeyMessage &msg, const Bytes &signingPrivateKey,
                             std::string &error) {
    proto::Writer w;
    w.fieldVarint(1, msg.id);
    w.fieldVarint(2, msg.iteration);
    if (!msg.ciphertext.empty()) w.fieldBytes(3, msg.ciphertext);

    Bytes signedPart{kVersion};
    const Bytes body = w.data();
    signedPart.insert(signedPart.end(), body.begin(), body.end());

    const Bytes signature = crypto::xeddsaSign(signingPrivateKey, signedPart);
    if (signature.size() != 64) {
        error = "xeddsa signing failed";
        return Bytes();
    }
    return concat(signedPart, signature);
}

bool decodeSenderKeyMessage(const Bytes &data, SenderKeyMessage &out, std::string &error) {
    out = SenderKeyMessage();
    if (data.size() < 1 + 64) {
        error = "sender key message too short";
        return false;
    }
    if (data[0] != kVersion) {
        error = "unsupported sender key message version";
        return false;
    }
    const size_t bodyLen = data.size() - 64;
    proto::Reader r(data.data() + 1, bodyLen - 1);
    proto::Field f;
    while (r.next(f)) {
        if (f.number == 1) out.id = uint32_t(f.varint);
        else if (f.number == 2) out.iteration = uint32_t(f.varint);
        else if (f.number == 3) out.ciphertext = f.bytes;
    }
    out.signature.assign(data.begin() + long(bodyLen), data.end());
    if (out.ciphertext.empty()) {
        error = "sender key message has no ciphertext";
        return false;
    }
    return true;
}

bool verifySenderKeyMessage(const SenderKeyMessage &msg, const Bytes &signedPart,
                            const Bytes &signingPublicKey, std::string &error) {
    const Bytes raw = rawPublicKey(signingPublicKey);
    if (raw.size() != 32) {
        error = "sender signing key has the wrong length";
        return false;
    }
    if (!crypto::xeddsaVerify(raw, signedPart, msg.signature)) {
        error = "sender key message signature did not verify";
        return false;
    }
    return true;
}

// ── Names ────────────────────────────────────────────────────────────────

std::string senderKeyName(const std::string &groupJid, const std::string &senderJid) {
    return groupJid + "::" + wa::jidNormalizedUser(senderJid);
}

// ── Sending ──────────────────────────────────────────────────────────────

bool createSenderKey(SenderKeyStore &store, const std::string &groupJid,
                     const std::string &meJid, SenderKeyDistribution &out,
                     std::string &error) {
    (void)error;   // creation cannot fail with a well-formed store
    const std::string name = senderKeyName(groupJid, meJid);
    SenderKeyState state;
    if (!store.loadSenderKey(name, state) || state.chainKey.size() != 32 ||
        !state.signingKey.valid()) {
        state = SenderKeyState();
        // keyhelper.generateSenderKeyId(): a positive 31-bit random id.
        const Bytes idBytes = crypto::randomBytes(4);
        const uint32_t id = (uint32_t(idBytes[0]) << 24 | uint32_t(idBytes[1]) << 16 |
                             uint32_t(idBytes[2]) << 8 | uint32_t(idBytes[3])) &
                            0x7fffffffu;
        state.keyId = id ? id : 1;
        state.chainKey = crypto::randomBytes(32);
        state.iteration = 0;
        state.signingKey = generateKeyPair();
        state.skipped.clear();
        store.storeSenderKey(name, state);
    }
    out.id = state.keyId;
    out.iteration = state.iteration;
    out.chainKey = state.chainKey;
    out.signingKey = state.signingKey.pub;
    return true;
}

bool encryptGroup(SenderKeyStore &store, const std::string &groupJid,
                  const std::string &meJid, const Bytes &plaintext, Bytes &skmsg,
                  std::string &error) {
    skmsg.clear();
    const std::string name = senderKeyName(groupJid, meJid);
    SenderKeyState state;
    if (!store.loadSenderKey(name, state) || state.chainKey.size() != 32 ||
        state.signingKey.priv.size() != 32) {
        error = "no sender key for " + groupJid + " (create one first)";
        return false;
    }

    const Bytes seed = derive(state.chainKey, 0x01);
    Bytes cipherKey, iv;
    if (!messageKeys(seed, cipherKey, iv)) {
        error = "cannot derive group message keys";
        return false;
    }

    SenderKeyMessage msg;
    msg.id = state.keyId;
    msg.iteration = state.iteration;
    msg.ciphertext = crypto::aes256CbcEncrypt(plaintext, cipherKey, iv);

    // NB: Baileys advances to iteration+1 for every message after the first
    // (a quirk of its GroupCipher). Receivers resynchronise by iteration, so
    // the plain 0,1,2… sequence below is both simpler and compatible.
    state.chainKey = derive(state.chainKey, 0x02);
    state.iteration += 1;
    store.storeSenderKey(name, state);

    skmsg = encodeSenderKeyMessage(msg, state.signingKey.priv, error);
    return !skmsg.empty();
}

// ── Receiving ────────────────────────────────────────────────────────────

bool processDistribution(SenderKeyStore &store, const std::string &groupJid,
                         const std::string &senderJid,
                         const SenderKeyDistribution &dist, std::string &error) {
    if (dist.chainKey.size() != 32) {
        error = "distribution message has no chain key";
        return false;
    }
    const std::string name = senderKeyName(groupJid, senderJid);
    SenderKeyState state;
    const bool have = store.loadSenderKey(name, state);
    if (have && state.keyId == dist.id && state.chainKey == dist.chainKey) {
        return true;   // already installed
    }
    SenderKeyState fresh;
    fresh.keyId = dist.id;
    fresh.chainKey = dist.chainKey;
    fresh.iteration = dist.iteration;
    fresh.signingKey.priv.clear();
    fresh.signingKey.pub = dist.signingKey.size() == 33
                               ? dist.signingKey
                               : concat(Bytes{0x05}, dist.signingKey);
    fresh.skipped.clear();
    store.storeSenderKey(name, fresh);
    return true;
}

bool decryptGroup(SenderKeyStore &store, const std::string &groupJid,
                  const std::string &senderJid, const Bytes &skmsg, Bytes &plaintext,
                  std::string &error) {
    plaintext.clear();
    const std::string name = senderKeyName(groupJid, senderJid);

    SenderKeyMessage msg;
    if (!decodeSenderKeyMessage(skmsg, msg, error)) return false;
    const Bytes signedPart(skmsg.begin(), skmsg.end() - 64);

    SenderKeyState state;
    if (!store.loadSenderKey(name, state)) {
        error = "no sender key for " + senderJid + " in " + groupJid +
                " (waiting for a distribution message)";
        return false;
    }
    if (state.keyId != msg.id) {
        error = "sender key id mismatch (" + compat::toString(msg.id) + " vs " +
                compat::toString(state.keyId) + ")";
        return false;
    }
    if (!verifySenderKeyMessage(msg, signedPart, state.signingKey.pub, error)) return false;

    Bytes seed;
    bool usedSkipped = false;
    if (state.iteration > msg.iteration) {
        auto it = state.skipped.find(msg.iteration);
        if (it == state.skipped.end()) {
            error = "group message iteration " + compat::toString(msg.iteration) +
                    " is older than the chain (" + compat::toString(state.iteration) +
                    ") and its key is gone";
            return false;
        }
        seed = it->second;
        state.skipped.erase(it);
        usedSkipped = true;
    } else {
        if (msg.iteration - state.iteration > kMaxFutureMessages) {
            error = "group message is more than 2000 messages into the future";
            return false;
        }
        while (state.iteration < msg.iteration) {
            state.skipped[state.iteration] = derive(state.chainKey, 0x01);
            state.chainKey = derive(state.chainKey, 0x02);
            state.iteration++;
            while (state.skipped.size() > kMaxSkippedKeys)
                state.skipped.erase(state.skipped.begin());
        }
        seed = derive(state.chainKey, 0x01);
        state.chainKey = derive(state.chainKey, 0x02);
        state.iteration++;
    }

    Bytes cipherKey, iv;
    if (!messageKeys(seed, cipherKey, iv)) {
        error = "cannot derive group message keys";
        return false;
    }
    if (!crypto::aes256CbcDecrypt(msg.ciphertext, cipherKey, iv, plaintext)) {
        error = "group message decryption failed (bad padding)";
        return false;
    }
    store.storeSenderKey(name, state);
    (void)usedSkipped;
    return true;
}

// ── Persistence ──────────────────────────────────────────────────────────

std::string serializeSenderKeyState(const SenderKeyState &state) {
    std::ostringstream ss;
    ss << state.keyId << ' ' << state.iteration << ' ' << hexOf(state.chainKey) << ' '
       << hexOf(state.signingKey.priv) << ' ' << hexOf(state.signingKey.pub);
    for (const auto &entry : state.skipped) {
        ss << " | " << entry.first << ' ' << hexOf(entry.second);
    }
    return ss.str();
}

bool deserializeSenderKeyState(const std::string &text, SenderKeyState &out) {
    out = SenderKeyState();
    std::istringstream head(text);
    std::string part;
    bool first = true;
    while (std::getline(head, part, '|')) {
        std::istringstream ss(part);
        if (first) {
            std::string chain, priv, pub;
            ss >> out.keyId >> out.iteration >> chain >> priv >> pub;
            out.chainKey = crypto::fromHex(chain);
            out.signingKey.priv = crypto::fromHex(priv);
            out.signingKey.pub = crypto::fromHex(pub);
            first = false;
            if (out.chainKey.size() != 32) return false;
            continue;
        }
        uint32_t iteration = 0;
        std::string seed;
        ss >> iteration >> seed;
        if (ss.fail()) continue;
        out.skipped[iteration] = crypto::fromHex(seed);
    }
    return out.chainKey.size() == 32;
}

} // namespace signal
} // namespace wa
