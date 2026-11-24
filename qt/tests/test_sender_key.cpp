// Group messaging (sender keys) test, in three phases.
//
//   phase 1  test_sender_key <vectors> <writeDir>
//              decodes a real SenderKeyDistributionMessage, decrypts Baileys'
//              own group messages (including its iteration skipping), exercises
//              the failure paths, then creates our sender key and writes an
//              skmsg + distribution message for the Node side.
//   phase 2  node qt/tests/verify_group.mjs <vectors> <dir>
//              Baileys' GroupCipher decrypts *our* skmsg, then a fresh sender
//              key on their side produces a distribution message + skmsg.
//   phase 3  test_sender_key <vectors> <dir> <dir>
//              installs that distribution message and decrypts their message,
//              and checks the plaintext Baileys recovered from ours.
//
// Build:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_sender_key.cpp
//       qt/src/direct/{sender_key,signal,ed25519,sha256,x25519,aes_gcm,proto_min,binary_node}.cpp

#include "sender_key.h"

#include "sha256.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace wa;
using Bytes = std::vector<uint8_t>;

static Bytes hex(const std::string &h) {
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}
static std::string str(const Bytes &b) { return std::string(b.begin(), b.end()); }

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}

// In-memory SenderKeyStore, the same shape the Qt client backs with creds.json.
class MemKeyStore : public signal::SenderKeyStore {
public:
    bool loadSenderKey(const std::string &name, signal::SenderKeyState &out) override {
        auto it = states_.find(name);
        if (it == states_.end()) return false;
        out = it->second;
        return true;
    }
    void storeSenderKey(const std::string &name, const signal::SenderKeyState &state) override {
        states_[name] = state;
    }
    std::map<std::string, signal::SenderKeyState> states_;
};

static const std::string kGroup = "120363041234567890@g.us";
static const std::string kBob = "5511@s.whatsapp.net";
static const std::string kMe = "919999999999@s.whatsapp.net";

int main(int argc, char **argv) {
    const std::string vectorsPath = argc > 1 ? argv[1] : "qt/tests/sender_key_vectors.txt";
    const std::string writeDir = argc > 2 ? argv[2] : "/tmp";
    const std::string readDir = argc > 3 ? argv[3] : std::string();

    std::ifstream in(vectorsPath);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", vectorsPath.c_str()); return 2; }

    // ── Parse the vectors ────────────────────────────────────────────────
    Bytes skdm, rekeySkdm;
    struct GroupMsg { uint32_t iteration; Bytes skmsg; Bytes plaintext; };
    std::vector<GroupMsg> msgs;
    Bytes badMsg;
    Bytes rekeyMsg;
    Bytes rekeyPlain;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "SKDM") {
            std::string id, iteration, chain, signing, serialized;
            ss >> id >> iteration >> chain >> signing >> serialized;
            skdm = hex(serialized);
        } else if (kind == "GROUPMSG") {
            std::string iteration, skmsg, plain;
            ss >> iteration >> skmsg >> plain;
            msgs.push_back({uint32_t(std::stoul(iteration)), hex(skmsg), hex(plain)});
        } else if (kind == "BADMSG") {
            std::string hexStr;
            ss >> hexStr;
            badMsg = hex(hexStr);
        } else if (kind == "REKEYSKDM") {
            std::string hexStr;
            ss >> hexStr;
            rekeySkdm = hex(hexStr);
        } else if (kind == "REKEYMSG") {
            std::string skmsg, plain;
            ss >> skmsg >> plain;
            rekeyMsg = hex(skmsg);
            rekeyPlain = hex(plain);
        }
    }
    check(!skdm.empty(), "vectors contain a distribution message");
    check(msgs.size() == 3, "three group messages");

    if (readDir.empty()) {
        // ── Phase 1 ───────────────────────────────────────────────────────
        signal::SenderKeyDistribution dist;
        std::string error;
        check(signal::decodeDistribution(skdm, dist, error),
              "decode SenderKeyDistributionMessage", error);
        check(dist.id != 0, "key id parsed", std::to_string(dist.id));
        check(dist.chainKey.size() == 32, "chain key is 32 bytes");
        check(dist.signingKey.size() == 33, "signing key is 33 bytes (0x05-prefixed)");
        check(dist.signingKey[0] == 0x05, "signing key has the bundle prefix");

        // Re-serialising must be byte-identical (we produce what WhatsApp sends).
        check(signal::encodeDistribution(dist) == skdm, "distribution re-encodes identically");

        MemKeyStore store;
        check(signal::processDistribution(store, kGroup, kBob, dist, error),
              "install distribution message", error);

        for (const auto &m : msgs) {
            Bytes plain;
            const bool ok = signal::decryptGroup(store, kGroup, kBob, m.skmsg, plain, error);
            check(ok, "decrypt group message", error);
            check(plain == m.plaintext, "group plaintext matches",
                  str(plain) + " != " + str(m.plaintext));
        }

        // The iteration-1 key was skipped by Baileys (0, 2, 4…): it must still
        // decrypt if it ever shows up, and our chain must be at iteration 5.
        check(store.states_[signal::senderKeyName(kGroup, kBob)].iteration >= 5,
              "chain advanced past the skipped iterations");
        check(!store.states_[signal::senderKeyName(kGroup, kBob)].skipped.empty(),
              "skipped message keys were cached");

        // Replaying a consumed message must fail (keys are single-use).
        {
            Bytes plain;
            std::string replayError;
            check(!signal::decryptGroup(store, kGroup, kBob, msgs.front().skmsg, plain,
                                        replayError),
                  "replayed message is rejected", replayError);
        }

        // Tampering with the signature must be detected.
        {
            Bytes plain;
            std::string badError;
            check(!signal::decryptGroup(store, kGroup, kBob, badMsg, plain, badError),
                  "bad signature is rejected", badError);
            check(badError.find("signature") != std::string::npos,
                  "corruption reported as a signature failure", badError);
        }

        // A message with a different key id belongs to another chain.
        {
            signal::SenderKeyMessage msg;
            std::string decodeError;
            check(signal::decodeSenderKeyMessage(msgs.back().skmsg, msg, decodeError),
                  "decode sender key message", decodeError);
            check(msg.iteration == 4, "message iteration parsed",
                  std::to_string(msg.iteration));
            msg.id += 1;
            const Bytes rekeyed = signal::encodeSenderKeyMessage(msg, Bytes(32, 1), decodeError);
            Bytes plain;
            std::string idError;
            check(!signal::decryptGroup(store, kGroup, kBob, rekeyed, plain, idError),
                  "wrong key id is rejected", idError);
        }

        // Truncated input must not crash or decode.
        {
            Bytes plain;
            std::string truncError;
            check(!signal::decryptGroup(store, kGroup, kBob,
                                        Bytes(msgs.front().skmsg.begin(),
                                              msgs.front().skmsg.begin() + 10),
                                        plain, truncError),
                  "truncated message rejected");
        }

        // ── Our own sender key ────────────────────────────────────────────
        MemKeyStore mine;
        signal::SenderKeyDistribution ourDist;
        check(signal::createSenderKey(mine, kGroup, kMe, ourDist, error),
              "create sender key", error);
        check(ourDist.chainKey.size() == 32, "our chain key is 32 bytes");
        check(ourDist.iteration == 0, "our chain starts at iteration 0");
        check(ourDist.signingKey.size() == 33, "our signing key is prefixed");

        // Creating twice must return the same chain (idempotent).
        {
            signal::SenderKeyDistribution again;
            check(signal::createSenderKey(mine, kGroup, kMe, again, error),
                  "create is idempotent", error);
            check(again.id == ourDist.id && again.chainKey == ourDist.chainKey,
                  "same key returned on second call");
        }

        const Bytes payload = hex("aabbccdd" "00112233");
        Bytes skmsg1, skmsg2;
        check(signal::encryptGroup(mine, kGroup, kMe, payload, skmsg1, error),
              "encrypt group message", error);
        check(signal::encryptGroup(mine, kGroup, kMe, payload, skmsg2, error),
              "encrypt second group message", error);

        signal::SenderKeyMessage m1, m2;
        std::string decodeError;
        check(signal::decodeSenderKeyMessage(skmsg1, m1, decodeError), "our skmsg decodes");
        check(signal::decodeSenderKeyMessage(skmsg2, m2, decodeError), "our second skmsg decodes");
        check(m1.iteration == 0 && m2.iteration == 1, "iterations advance by one");

        // A receiver that installs our distribution message must decrypt both.
        {
            MemKeyStore peer;
            signal::SenderKeyDistribution theirView;
            check(signal::decodeDistribution(signal::encodeDistribution(ourDist), theirView, error),
                  "our distribution message round-trips");
            check(signal::processDistribution(peer, kGroup, kMe, theirView, error),
                  "peer installs our distribution", error);
            Bytes plain1, plain2;
            check(signal::decryptGroup(peer, kGroup, kMe, skmsg1, plain1, error),
                  "peer decrypts our first message", error);
            check(plain1 == payload, "peer plaintext matches");
            check(signal::decryptGroup(peer, kGroup, kMe, skmsg2, plain2, error),
                  "peer decrypts our second message", error);
        }

        // State serialisation (creds.json persistence).
        {
            const auto &state = mine.states_[signal::senderKeyName(kGroup, kMe)];
            signal::SenderKeyState restored;
            check(signal::deserializeSenderKeyState(signal::serializeSenderKeyState(state),
                                                    restored),
                  "state serialises and parses");
            check(restored.keyId == state.keyId && restored.chainKey == state.chainKey &&
                      restored.iteration == state.iteration,
                  "state round-trips");
        }

        std::ofstream out(writeDir + "/cpp_group.txt");
        out << "CPPSKDM " << wa::crypto::toHex(signal::encodeDistribution(ourDist)) << "\n";
        out << "CPPSKMSG " << wa::crypto::toHex(skmsg1) << "\n";
        out << "CPPPAYLOAD " << wa::crypto::toHex(payload) << "\n";
        out.close();

        std::printf("\n=== Sender keys (phase 1) ===\n");
        std::printf("%d passed, %d failed\n", pass, fail);
        std::printf("%s\n", fail == 0 ? "GROUP BUILD OK" : "GROUP BUILD FAILED");
        return fail == 0 ? 0 : 1;
    }

    // ── Phase 3: Baileys' side of the conversation ──────────────────────
    std::ifstream nodeFile(readDir + "/node_group.txt");
    check(static_cast<bool>(nodeFile), "node_group.txt exists");
    if (!nodeFile) return 1;

    Bytes theirSkdm, theirMsg, theirPlain, ourPayload, nodeDecrypted;
    while (std::getline(nodeFile, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind, a, b;
        ss >> kind;
        if (kind == "NODESKDM") { ss >> a; theirSkdm = hex(a); }
        else if (kind == "NODEGROUPMSG") { ss >> a >> b; theirMsg = hex(a); theirPlain = hex(b); }
        else if (kind == "NODEDECRYPTED") { ss >> a; nodeDecrypted = hex(a); }
    }

    std::ifstream ours(readDir + "/cpp_group.txt");
    while (std::getline(ours, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind, a;
        ss >> kind;
        if (kind == "CPPPAYLOAD") { ss >> a; ourPayload = hex(a); }
    }

    // Baileys' GroupCipher decrypted the message we produced.
    check(!nodeDecrypted.empty(), "Baileys decrypted our group message");
    check(nodeDecrypted == ourPayload, "plaintext recovered by Baileys matches");

    // A distribution message created by Baileys' own builder decrypts for us.
    check(!theirSkdm.empty(), "node distribution message present");
    signal::SenderKeyDistribution nodeDist;
    std::string error;
    check(signal::decodeDistribution(theirSkdm, nodeDist, error),
          "decode the node distribution message", error);

    MemKeyStore store;
    check(signal::processDistribution(store, kGroup, kBob, nodeDist, error),
          "install node distribution", error);
    Bytes plain;
    check(signal::decryptGroup(store, kGroup, kBob, theirMsg, plain, error),
          "decrypt the node group message", error);
    check(plain == theirPlain, "node group plaintext matches", str(plain));

    std::printf("\n=== Sender keys (phase 3) ===\n");
    std::printf("%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "GROUP INTEROP OK" : "GROUP INTEROP FAILED");
    return fail == 0 ? 0 : 1;
}
