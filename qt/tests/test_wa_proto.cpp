// WAProto subset test — validates the hand-written Message / WebMessageInfo /
// HistorySync codecs against vectors produced by Baileys' protobufjs build.
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_wa_proto.cpp
//       qt/src/direct/wa_proto.cpp qt/src/direct/proto_min.cpp -o /tmp/test_wa_proto
//   /tmp/test_wa_proto qt/tests/wa_proto_vectors.txt

#include "wa_proto.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;
using Bytes = std::vector<uint8_t>;

static Bytes hex(const std::string &h) {
    if (h == "-") return Bytes();
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static std::string str(const Bytes &b) { return std::string(b.begin(), b.end()); }

static std::string toHex(const Bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xf]; }
    return s;
}

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/wa_proto_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    std::string line;
    int msgs = 0, hist = 0, notes = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "MSG_CONV") {
            std::string wireHex, textHex;
            ss >> wireHex >> textHex;
            const Bytes wire = hex(wireHex);

            // Decode what protobufjs produced.
            waproto::Message decoded;
            check(waproto::decodeMessage(wire, decoded), "decode conversation");
            check(decoded.conversation == str(hex(textHex)), "conversation text");

            // Re-encode: must be byte-identical.
            waproto::Message m;
            m.conversation = str(hex(textHex));
            check(waproto::encodeMessage(m) == wire, "encode conversation parity",
                  toHex(waproto::encodeMessage(m)) + " != " + wireHex);
            msgs++;
        } else if (kind == "MSG_EXT") {
            std::string wireHex, textHex, quotedHex, stanzaId, participant;
            ss >> wireHex >> textHex >> quotedHex >> stanzaId >> participant;
            const Bytes wire = hex(wireHex);

            waproto::Message decoded;
            check(waproto::decodeMessage(wire, decoded), "decode extended text");
            check(decoded.hasExtendedText, "extended text present");
            check(decoded.extendedText == str(hex(textHex)), "extended text body");
            check(decoded.image.hasContextInfo, "context info present");
            check(decoded.image.contextInfo.stanzaId == stanzaId, "quoted stanza id");
            check(decoded.image.contextInfo.participant == participant, "quoted participant");
            check(decoded.image.contextInfo.quotedMessage == hex(quotedHex), "quoted payload");

            // The quoted message itself must decode to the original text.
            waproto::Message quoted;
            check(waproto::decodeMessage(decoded.image.contextInfo.quotedMessage, quoted),
                  "decode quoted message");
            check(quoted.conversation == "the original", "quoted text");

            // Re-encode and compare.
            waproto::Message rebuilt;
            rebuilt.hasExtendedText = true;
            rebuilt.extendedText = str(hex(textHex));
            rebuilt.image.hasContextInfo = true;
            rebuilt.image.contextInfo.stanzaId = stanzaId;
            rebuilt.image.contextInfo.participant = participant;
            rebuilt.image.contextInfo.quotedMessage = hex(quotedHex);
            check(waproto::encodeMessage(rebuilt) == wire, "encode extended text parity",
                  toHex(waproto::encodeMessage(rebuilt)) + " != " + wireHex);
            msgs++;
        } else if (kind == "MSG_IMG") {
            std::string wireHex, captionHex, mime, lengthStr, mediaKeyHex, directPath;
            ss >> wireHex >> captionHex >> mime >> lengthStr >> mediaKeyHex >> directPath;
            const Bytes wire = hex(wireHex);

            waproto::Message m;
            check(waproto::decodeMessage(wire, m), "decode image");
            check(m.mediaType() == "image", "image media type");
            check(m.image.caption == str(hex(captionHex)), "image caption");
            check(m.image.mimetype == mime, "image mimetype");
            check(m.image.fileLength == std::stoull(lengthStr), "image length");
            check(m.image.mediaKey == hex(mediaKeyHex), "image media key");
            check(m.image.directPath == directPath, "image direct path");
            check(m.image.jpegThumbnail.size() == 4, "image thumbnail");
            msgs++;
        } else if (kind == "HISTORY") {
            std::string wireHex;
            uint32_t progress, convCount;
            std::string msg1Id, msg1TextHex, convName, convJid, msg3Id, msg3TextHex;
            std::string pushJid, pushNameHex;
            ss >> wireHex >> progress >> convCount >> msg1Id >> msg1TextHex >> convName >>
                convJid >> msg3Id >> msg3TextHex >> pushJid >> pushNameHex;

            waproto::HistorySync hs;
            check(waproto::decodeHistorySync(hex(wireHex), hs), "decode HistorySync");
            check(hs.progress == progress, "history progress");
            check(hs.conversations.size() == convCount, "conversation count");

            bool foundGroup = false, foundDirect = false;
            for (const auto &c : hs.conversations) {
                if (c.id == convJid) {
                    foundGroup = true;
                    check(c.name == convName, "group name");
                    check(c.unreadCount == 3, "group unread count");
                    check(c.messages.size() == 2, "group message count");
                    if (c.messages.size() == 2) {
                        check(c.messages[0].key.id == msg1Id, "first group message id");
                        check(c.messages[0].message.conversation == str(hex(msg1TextHex)),
                              "first group message text");
                        check(!c.messages[0].key.fromMe, "first group message direction");
                        check(c.messages[0].key.participant == "918888888888@s.whatsapp.net",
                              "group participant");
                        check(c.messages[1].key.fromMe, "second group message direction");
                        check(c.messages[1].messageTimestamp == 1699999500ULL, "message timestamp");
                    }
                }
                for (const auto &m : c.messages) {
                    if (m.key.id == msg3Id) {
                        foundDirect = true;
                        check(m.message.conversation == str(hex(msg3TextHex)),
                              "direct message text");
                        check(m.messageTimestamp == 1700000500ULL, "direct timestamp");
                    }
                }
            }
            check(foundGroup, "group conversation present");
            check(foundDirect, "direct conversation present");
            check(hs.pushnames.size() == 2, "pushname count");
            for (const auto &pn : hs.pushnames) {
                if (pn.id == pushJid) check(pn.pushname == str(hex(pushNameHex)), "pushname text");
            }
            hist++;
        } else if (kind == "NOTIFY") {
            std::string wireHex, mediaKeyHex, directPath;
            uint64_t fileLength;
            uint32_t syncType, chunkOrder, progress;
            ss >> wireHex >> fileLength >> mediaKeyHex >> directPath >> syncType >>
                chunkOrder >> progress;

            waproto::HistorySyncNotification n;
            check(waproto::decodeHistorySyncNotification(hex(wireHex), n), "decode notification");
            check(n.fileLength == fileLength, "notification length");
            check(n.mediaKey == hex(mediaKeyHex), "notification media key");
            check(n.directPath == directPath, "notification direct path");
            check(n.syncType == syncType, "notification sync type");
            check(n.chunkOrder == chunkOrder, "notification chunk order");
            check(n.progress == progress, "notification progress");
            notes++;
        }
    }

    // Padding round-trip. WhatsApp pads with a random 1..16 byte tail (Baileys'
    // writeRandomPadMax16), so the result is *not* block aligned and only the
    // trailing length byte may be trusted.
    for (size_t len : {0u, 1u, 15u, 16u, 17u, 200u}) {
        const Bytes raw(len, 'x');
        const Bytes padded = waproto::padMessage(raw);
        Bytes unpadded;
        const size_t padLen = padded.size() - raw.size();
        check(padLen >= 1 && padLen <= 16, "pad length in 1..16");
        check(waproto::unpadMessage(padded, unpadded) && unpadded == raw, "padding round-trip");
    }

    std::printf("\n=== WAProto messages/history ===\n");
    std::printf("Message vectors      : %d\n", msgs);
    std::printf("HistorySync vectors  : %d\n", hist);
    std::printf("Notification vectors : %d\n", notes);
    std::printf("\n%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL WAPROTO TESTS PASSED" : "WAPROTO TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
