// Signal Protocol conformance test — the C++ port against real libsignal.
//
// Two-phase interop test:
//   test_signal <vectors> <cppMessageOut> [replyIn]
//   1. decrypts messages encrypted by libsignal's SessionCipher
//   2. writes its own PreKeyWhisperMessage to <cppMessageOut>
//   3. (second run) decrypts libsignal's reply loaded from [replyIn]
// The Node side of phases 2-3 lives in qt/tests/verify_signal.mjs; run_tests.sh
// drives the whole exchange, so our ciphertext really is accepted by libsignal
// and its reply really is decrypted here.
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_signal.cpp
//       qt/src/direct/signal.cpp qt/src/direct/ed25519.cpp qt/src/direct/sha256.cpp
//       qt/src/direct/x25519.cpp qt/src/direct/aes_gcm.cpp qt/src/direct/proto_min.cpp
//       -o /tmp/test_signal
//   /tmp/test_signal qt/tests/signal_vectors.txt

#include "signal.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;
using Bytes = std::vector<uint8_t>;
using signal::Bytes;

static Bytes hex(const std::string &h) {
    if (h == "-") return Bytes();
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static std::string toHex(const Bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xf]; }
    return s;
}

// In-memory store, standing in for the Qt layer's JSON-backed store.
class MemStore : public signal::Store {
public:
    signal::KeyPair identity;
    uint32_t regId = 0;
    std::map<uint32_t, signal::KeyPair> preKeys;
    std::map<uint32_t, signal::KeyPair> signedPreKeys;
    std::map<std::string, signal::Record> sessions;

    signal::KeyPair ourIdentity() override { return identity; }
    uint32_t ourRegistrationId() override { return regId; }
    bool loadPreKey(uint32_t id, signal::KeyPair &out) override {
        auto it = preKeys.find(id);
        if (it == preKeys.end()) return false;
        out = it->second;
        return true;
    }
    void removePreKey(uint32_t id) override { preKeys.erase(id); }
    bool loadSignedPreKey(uint32_t id, signal::KeyPair &out) override {
        auto it = signedPreKeys.find(id);
        if (it == signedPreKeys.end()) return false;
        out = it->second;
        return true;
    }
    signal::Record loadSession(const std::string &address) override {
        auto it = sessions.find(address);
        return it == sessions.end() ? signal::Record() : it->second;
    }
    void storeSession(const std::string &address, const signal::Record &record) override {
        sessions[address] = record;
    }
};

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/signal_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    MemStore bob;
    MemStore aliceStore;
    std::string line;
    int pass = 0, fail = 0;
    struct JsMsg { int type; Bytes body; Bytes text; };
    std::vector<JsMsg> jsMessages;
    signal::EncryptResult ourMessage;

    auto check = [&](bool ok, const char *what, const std::string &extra = "") {
        if (ok) { pass++; return; }
        fail++;
        std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "BOBKEY") {
            std::string privHex, pubHex;
            uint32_t reg;
            ss >> privHex >> pubHex >> reg;
            bob.identity.priv = hex(privHex);
            bob.identity.pub = hex(pubHex);
            bob.regId = reg;
        } else if (kind == "BOBSIGNED") {
            uint32_t id;
            std::string privHex, pubHex, sigHex;
            ss >> id >> privHex >> pubHex >> sigHex;
            signal::KeyPair kp;
            kp.priv = hex(privHex);
            kp.pub = hex(pubHex);
            bob.signedPreKeys[id] = kp;
        } else if (kind == "BOBPREKEY") {
            uint32_t id;
            std::string privHex, pubHex;
            ss >> id >> privHex >> pubHex;
            signal::KeyPair kp;
            kp.priv = hex(privHex);
            kp.pub = hex(pubHex);
            bob.preKeys[id] = kp;
        } else if (kind == "BOBBUNDLE") {
            std::string idHex;
            uint32_t reg, signedId, preKeyId;
            std::string signedPubHex, sigHex, preKeyPubHex;
            ss >> idHex >> reg >> signedId >> signedPubHex >> sigHex >> preKeyId >> preKeyPubHex;

            signal::Bundle bundle;
            bundle.identityKey = hex(idHex);
            bundle.registrationId = reg;
            bundle.signedPreKey.id = signedId;
            bundle.signedPreKey.pair.pub = hex(signedPubHex);
            bundle.signedPreKey.signature = hex(sigHex);
            bundle.hasPreKey = true;
            bundle.preKey.id = preKeyId;
            bundle.preKey.pair.pub = hex(preKeyPubHex);

            // ── Direction 1: Alice = C++ here. Encrypt to Bob with our port ──
            MemStore &alice = aliceStore;
            alice = MemStore();
            alice.identity = signal::generateKeyPair();
            alice.regId = 1234;

            std::string err;
            check(signal::initOutgoingSession(alice, "bob", bundle, err),
                  "initOutgoingSession", err);

            const Bytes plaintext{'h', 'i', ' ', 'b', 'o', 'b'};
            auto enc = signal::encrypt(alice, "bob", plaintext);
            check(enc.ok, "encrypt", enc.error);
            check(enc.type == 3, "first message must be a PreKeyWhisperMessage");
            check(!enc.body.empty(), "ciphertext non-empty");

            ourMessage = enc;

            // Round-trip through our own store first (self-consistency).
            MemStore bobCopy = bob;
            Bytes decrypted;
            check(signal::decrypt(bobCopy, "alice", enc.type, enc.body, decrypted, err),
                  "decrypt our own pre-key message", err);
            check(decrypted == plaintext, "plaintext mismatch (self round-trip)");
        } else if (kind == "JSMSG") {
            JsMsg m;
            std::string typeStr, bodyHex, textHex;
            ss >> typeStr >> bodyHex >> textHex;
            m.type = std::stoi(typeStr);
            m.body = hex(bodyHex);
            m.text = hex(textHex);
            jsMessages.push_back(m);
        }
    }

    // ── Direction 2: messages produced by libsignal (Alice = JS) ──────────
    MemStore bobSide = bob;
    for (const auto &m : jsMessages) {
        Bytes out;
        std::string err;
        check(signal::decrypt(bobSide, "alice", m.type, m.body, out, err),
              "decrypt libsignal message", err);
        check(out == m.text, "libsignal plaintext mismatch",
              std::string("got \"") + std::string(out.begin(), out.end()) + "\"");
    }

    // ── Phase 2: hand our ciphertext to the Node verifier ─────────────────
    // The session state travels with it, exactly as the real app persists it,
    // so phase 3 can resume the same session (and this exercises the
    // serialize/deserialize path against a live libsignal peer).
    // Only the first invocation writes (a second run would otherwise overwrite
    // the ciphertext whose reply it is about to decrypt).
    if (argc == 3) {
        std::ofstream out(argv[2]);
        if (out) {
            out << "CPPMSG " << ourMessage.type << " " << toHex(ourMessage.body) << "\n";
            out << "CPPIDENTITY " << toHex(aliceStore.identity.priv) << " "
                << toHex(aliceStore.identity.pub) << " " << aliceStore.regId << "\n";
            out << "CPPSTATE " << toHex(signal::serializeRecord(aliceStore.sessions["bob"])) << "\n";
            check(!ourMessage.body.empty(), "our ciphertext written for libsignal");
        }
    }

    // ── Phase 3: decrypt libsignal's reply to our message ─────────────────
    if (argc > 3) {
        // Restore the session written in phase 2.
        {
            std::ifstream stateIn(argv[2]);
            std::string stateLine;
            while (stateIn && std::getline(stateIn, stateLine)) {
                std::istringstream rs(stateLine);
                std::string kind;
                rs >> kind;
                if (kind == "CPPIDENTITY") {
                    std::string privHex, pubHex;
                    uint32_t reg = 0;
                    rs >> privHex >> pubHex >> reg;
                    aliceStore.identity.priv = hex(privHex);
                    aliceStore.identity.pub = hex(pubHex);
                    aliceStore.regId = reg;
                } else if (kind == "CPPSTATE") {
                    std::string blobHex;
                    rs >> blobHex;
                    signal::Record restored;
                    if (!signal::deserializeRecord(hex(blobHex), restored)) {
                        check(false, "restore session state");
                        continue;
                    }
                    aliceStore.sessions["bob"] = restored;
                    check(true, "restore session state");
                }
            }
        }
        std::ifstream replyIn(argv[3]);
        std::string replyLine;
        if (replyIn && std::getline(replyIn, replyLine) && !replyLine.empty()) {
            std::istringstream rs(replyLine);
            std::string kind, typeStr, bodyHex, textHex;
            rs >> kind >> typeStr >> bodyHex >> textHex;
            if (kind == "JSREPLY") {
                // The reply is addressed to the same C++ "alice" identity, so we
                // have to rebuild her store state from the bundle branch above.
                Bytes out;
                std::string err;
                check(signal::decrypt(aliceStore, "bob", std::stoi(typeStr), hex(bodyHex),
                                      out, err),
                      "decrypt libsignal reply", err);
                const Bytes want = hex(textHex);
                check(out == want, "libsignal reply plaintext mismatch",
                      std::string("got \"") + std::string(out.begin(), out.end()) + "\"");
            }
        }
    }

    // ── Serialisation round-trip ──────────────────────────────────────────
    {
        const auto it = bobSide.sessions.find("alice");
        if (it != bobSide.sessions.end()) {
            const Bytes blob = signal::serializeRecord(it->second);
            signal::Record restored;
            check(signal::deserializeRecord(blob, restored), "deserializeRecord");
            check(restored.sessions.size() == it->second.sessions.size(),
                  "session count after round-trip");
            check(signal::serializeRecord(restored) == blob, "record round-trip is stable");
        }
    }

    std::printf("\n=== Signal Protocol (vs libsignal) ===\n");
    std::printf("%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL SIGNAL TESTS PASSED" : "SIGNAL TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
