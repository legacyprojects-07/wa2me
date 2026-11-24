// End-to-end test of the message layer (wa_message.cpp) against libsignal.
//
// The three-phase dance mirrors test_signal.cpp because a Signal session has
// to exist on both sides:
//
//   phase 1  test_message <vectors> <writeDir>
//              parses the server's key-response BinaryNode, generates our own
//              identity/pre-keys, encrypts "hello from qt" for Bob and writes
//              the stanza plus our public bundle.
//   phase 2  node qt/tests/verify_message.mjs <vectors> <writeDir>
//              libsignal decrypts our <enc>, then encrypts a reply as a
//              <message> stanza (including a deviceSentMessage copy).
//   phase 3  test_message <vectors> <writeDir> <readDir>
//              decrypts those stanzas and checks chat/text routing.
//
// Build:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_message.cpp qt/src/direct/{wa_message,wa_proto,signal,ed25519,sha256,x25519,aes_gcm,proto_min,binary_node,inflate}.cpp
//      -o /tmp/test_message

#include "binary_node.h"
#include "ed25519.h"
#include "message_test_store.h"
#include "wa_message.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;
using Bytes = std::vector<uint8_t>;

static Bytes hex(const std::string &h) {
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}
static std::string toHex(const Bytes &b) { return crypto::toHex(b); }

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}

static const std::string kMeJid = "919999999999@s.whatsapp.net";
static const std::string kBobJid = "5511@s.whatsapp.net";
static const std::string kBobDevice = "5511:0@s.whatsapp.net";
static const std::string kText = "hello from qt";

static std::string readFile(const std::string &path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static const BinaryNode *firstToNode(const BinaryNode &stanza) {
    const BinaryNode *participants = stanza.child("participants");
    if (!participants) return nullptr;
    for (const BinaryNode *to : participants->childrenNamed("to")) return to;
    return nullptr;
}

int main(int argc, char **argv) {
    const std::string vectorsPath = argc > 1 ? argv[1] : "qt/tests/message_vectors.txt";
    const std::string writeDir = argc > 2 ? argv[2] : "/tmp";
    const std::string readDir = argc > 3 ? argv[3] : std::string();

    std::ifstream in(vectorsPath);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", vectorsPath.c_str()); return 2; }

    Bytes bundleFrame;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "KEYRESP") {
            std::string frameHex;
            ss >> frameHex;
            bundleFrame = hex(frameHex);
        }
    }
    if (bundleFrame.empty()) {
        std::fprintf(stderr, "no KEYRESP vector\n");
        return 2;
    }

    test::TestStore store;

    if (readDir.empty()) {
        // ── Phase 1: our keys, parse the bundle, encrypt for Bob ──────────
        const signal::KeyPair identity = signal::generateKeyPair();
        const signal::KeyPair signedPreKey = signal::generateKeyPair();
        const signal::KeyPair preKey = signal::generateKeyPair();
        const Bytes signature = crypto::xeddsaSign(identity.priv, signedPreKey.pub);

        store.setIdentity(identity, 1234);
        store.addSignedPreKey(5, signal::PreKeyRecord{5, signedPreKey, signature});
        store.addPreKey(4242, preKey);

        // The key response arrives as a full BinaryNode frame off the wire.
        const binary::DecodeResult decoded = binary::decode(bundleFrame);
        check(decoded.ok, "decode key-response frame", decoded.error);
        if (!decoded.ok) return 1;

        std::vector<messaging::DeviceBundle> bundles;
        std::string error;
        check(messaging::parseKeyResponse(decoded.node, bundles, error),
              "parse key response", error);
        check(bundles.size() == 1, "one device bundle");
        if (bundles.empty()) return 1;

        const auto &db = bundles.front();
        check(db.jid == kBobJid, "device jid (device 0 has no suffix)", db.jid);
        check(db.bundle.identityKey.size() == 33, "identity key prefixed");
        check(db.bundle.signedPreKey.pair.pub.size() == 33, "signed pre-key prefixed");
        check(db.bundle.signedPreKey.signature.size() == 64, "signed pre-key signature");
        check(db.bundle.hasPreKey, "one-time pre-key present");
        check(db.bundle.registrationId != 0, "registration id parsed");
        check(!db.bundle.signedPreKey.pair.priv.size(), "bundles carry no private keys");

        messaging::SendPlan plan;
        check(messaging::buildTextStanza(store, kMeJid, kBobJid, kText, "MSGID1", bundles,
                                         plan, error),
              "build text stanza", error);
        check(plan.stanza.tag == "message", "stanza tag");
        check(plan.stanza.attrOr("type") == "text", "stanza type");
        check(plan.stanza.attrOr("to") == kBobJid, "stanza recipient");
        check(plan.stanza.attrOr("id") == "MSGID1", "stanza id");
        check(plan.deviceCount == 1, "device count");
        check(plan.usedPreKey, "first message is a pre-key message");

        const BinaryNode *to = firstToNode(plan.stanza);
        check(to != nullptr, "participant node");
        if (to) {
            check(to->attrOr("jid") == kBobJid, "participant jid");
            const BinaryNode *enc = to->child("enc");
            check(enc != nullptr, "enc node");
            if (enc) {
                check(enc->attrOr("v") == "2", "enc v=2");
                check(enc->attrOr("type") == "pkmsg", "enc type pkmsg");
                check(enc->bytes.size() > 32, "ciphertext non-empty");
            }
        }

        // The wire frame must survive our own encoder (this is what sendNode sends).
        const Bytes wire = binary::encode(plan.stanza);
        const binary::DecodeResult roundTrip = binary::decode(wire);
        check(roundTrip.ok, "stanza re-decodes", roundTrip.error);

        // Second send on the same session must be a plain whisper message.
        messaging::SendPlan plan2;
        check(messaging::buildTextStanza(store, kMeJid, kBobJid, "second", "MSGID2", bundles,
                                         plan2, error),
              "second stanza", error);
        check(plan2.usedPreKey, "pre-key repeated until the peer answers (libsignal)");

        // Own-device copies get the deviceSentMessage wrapper.
        BinaryNode wrapped;
        bool wrappedPreKey = false;
        waproto::Message textMsg;
        textMsg.conversation = kText;
        check(messaging::encryptForDevice(store, bundles.front(), textMsg, true, kBobJid,
                                          wrapped, wrappedPreKey, error),
              "encrypt wrapped", error);
        check(wrapped.tag == "to" && wrapped.child("enc") != nullptr, "wrapped node shape");
        check(wrappedPreKey, "wrapped copy also uses the pre-key message");

        std::ofstream out(writeDir + "/cpp_message.txt");
        out << "CPPSTANZA " << toHex(wire) << "\n";
        out << "CPPWRAPPED " << toHex(binary::encode(wrapped)) << "\n";
        out << "CPPIDENTITY " << toHex(identity.priv) << " " << toHex(identity.pub) << " 1234\n";
        out << "CPPSIGNED 5 " << toHex(signedPreKey.pub) << " " << toHex(signature) << "\n";
        out << "CPPPREKEY 4242 " << toHex(preKey.pub) << "\n";
        out.close();
        store.save(writeDir + "/cpp_message_store.txt");

        std::printf("\n=== Message layer (phase 1: build) ===\n");
        std::printf("%d passed, %d failed\n", pass, fail);
        std::printf("%s\n", fail == 0 ? "MESSAGE BUILD OK" : "MESSAGE BUILD FAILED");
        return fail == 0 ? 0 : 1;
    }

    // ── Phase 3: decrypt what libsignal sent us ──────────────────────────
    store.load(readDir + "/cpp_message_store.txt");

    const std::string stanzaHex = readFile(readDir + "/node_message.txt");
    check(!stanzaHex.empty(), "node message exists");
    if (fail) return 1;

    std::istringstream lines(stanzaHex);
    std::string kind, hexPayload;
    std::getline(lines, kind, ' ');
    std::getline(lines, hexPayload, '\n');
    check(kind == "NODESTANZA", "node stanza kind", kind);

    const binary::DecodeResult decoded = binary::decode(hex(hexPayload));
    check(decoded.ok, "decode node stanza", decoded.error);
    if (!decoded.ok) return 1;

    messaging::IncomingMessage msg;
    std::string error;
    check(messaging::decryptMessageNode(store, decoded.node, kMeJid, msg, error),
          "decrypt node message", error);
    check(msg.decrypted, "message decrypted");
    check(msg.chatJid == kBobJid, "chat jid", msg.chatJid);
    check(msg.senderJid == kBobJid, "sender jid", msg.senderJid);
    check(!msg.fromMe, "not from me");
    check(msg.id == "NODE1", "message id", msg.id);
    check(msg.text == "hello from node", "message text", msg.text);
    check(msg.timestamp == 1700001234ULL, "timestamp");

    // A second stanza from the same session (plain whisper message).
    std::string hex2;
    {
        std::istringstream rest(readFile(readDir + "/node_message2.txt"));
        std::string k2;
        std::getline(rest, k2, ' ');
        std::getline(rest, hex2, '\n');
    }
    if (!hex2.empty()) {
        const binary::DecodeResult d2 = binary::decode(hex(hex2));
        check(d2.ok, "decode second stanza", d2.error);
        if (d2.ok) {
            messaging::IncomingMessage msg2;
            check(messaging::decryptMessageNode(store, d2.node, kMeJid, msg2, error),
                  "decrypt second stanza", error);
            check(msg2.text == "second from node", "second text", msg2.text);
        }
    }

    // deviceSentMessage wrapper: our own device copy carries the real chat.
    const std::string wrappedHex = readFile(readDir + "/node_wrapped.txt");
    if (!wrappedHex.empty()) {
        std::istringstream ws(wrappedHex);
        std::string k3, payload;
        std::getline(ws, k3, ' ');
        std::getline(ws, payload, '\n');
        const binary::DecodeResult d3 = binary::decode(hex(payload));
        check(d3.ok, "decode wrapped stanza", d3.error);
        if (d3.ok) {
            messaging::IncomingMessage msg3;
            check(messaging::decryptMessageNode(store, d3.node, kMeJid, msg3, error),
                  "decrypt wrapped stanza", error);
            check(msg3.text == "hello from qt", "wrapped inner text", msg3.text);
            check(msg3.chatJid == kBobJid, "wrapped destination jid", msg3.chatJid);
        }
    }

    std::printf("\n=== Message layer (phase 3: decrypt) ===\n");
    std::printf("%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "MESSAGE DECRYPT OK" : "MESSAGE DECRYPT FAILED");
    return fail == 0 ? 0 : 1;
}
