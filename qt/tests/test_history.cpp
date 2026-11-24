// HistorySync + media-crypto test.
//
// The vectors are produced by Baileys' own code path (Node crypto for HKDF /
// AES-CBC / HMAC, zlib for compression, protobufjs for the proto), so this
// proves the C++ client can decrypt and decode a real history sync payload.
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_history.cpp
//       qt/src/direct/{wa_history,media_crypto,wa_proto,proto_min,inflate,
//                      aes_gcm,sha256}.cpp -o /tmp/test_history
//   /tmp/test_history qt/tests/history_vectors.txt

#include "inflate.h"
#include "media_crypto.h"
#include "sha256.h"
#include "wa_history.h"

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

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}

static const history::Chat *findChat(const history::SyncData &data, const std::string &jid) {
    for (const auto &c : data.chats)
        if (c.jid == jid) return &c;
    return nullptr;
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/history_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    std::string line;
    history::SyncData merged;
    int historyChunks = 0, mediaBlobs = 0, pushChunks = 0;

    // zlib inflate must reject junk instead of returning garbage.
    {
        const Bytes junk = {'h', 'e', 'l', 'l', 'o', ' ', 'z', 'l', 'i', 'b'};
        Bytes out;
        check(!binary::inflateZlib(junk, out), "inflate rejects non-zlib input");
    }

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "HISTORY" || kind == "PUSHNAMES") {
            std::string blobHex, keyHex;
            ss >> blobHex >> keyHex;
            uint32_t syncType = 0, progress = 0;
            ss >> syncType >> progress;

            history::SyncData data;
            std::string error;
            const bool ok = history::decode(hex(blobHex), hex(keyHex), data, error);
            check(ok, "history blob decrypt+decode", error);
            if (!ok) continue;

            check(data.syncType == syncType, "sync type");
            check(data.progress == progress, "progress");
            merged.merge(data);

            if (kind == "HISTORY") {
                historyChunks++;
                check(data.chats.size() == 2, "chat count");

                const history::Chat *direct = findChat(data, "919999999999@s.whatsapp.net");
                check(direct != nullptr, "direct chat present");
                if (direct) {
                    check(direct->unreadCount == 2, "direct unread count");
                    check(direct->lastMessageTimestamp == 1700000700ULL, "direct timestamp");
                    check(direct->messages.size() == 2, "direct message count");
                    if (direct->messages.size() == 2) {
                        check(direct->messages[0].key.id == "HIST1", "first id");
                        check(!direct->messages[0].key.fromMe, "first direction");
                        check(direct->messages[0].message.conversation == "older message",
                              "conversation text");
                        check(direct->messages[0].messageTimestamp == 1700000600ULL,
                              "message timestamp");
                        check(direct->messages[1].key.id == "HIST2", "second id");
                        check(direct->messages[1].key.fromMe, "second direction");
                        check(direct->messages[1].message.hasExtendedText, "extended text flag");
                        check(direct->messages[1].message.extendedText == "newer reply",
                              "extended text body");
                    }
                }

                const history::Chat *group = findChat(data, "120363041234567890@g.us");
                check(group != nullptr, "group chat present");
                if (group) {
                    check(group->name == "Family", "group subject");
                    check(group->messages.size() == 1, "group message count");
                    if (!group->messages.empty()) {
                        check(group->messages[0].key.participant == "918888888888@s.whatsapp.net",
                              "group participant");
                        check(group->messages[0].message.conversation == "group hello",
                              "group text");
                    }
                }
                check(data.messageCount() == 3, "flattened message count");
            } else {
                pushChunks++;
                check(data.contacts.size() == 2, "pushname contacts");
            }
        } else if (kind == "HISTORY_TEXT") {
            // Same payload, already decrypted and inflated: parser only.
            std::string protoHex;
            ss >> protoHex;
            history::SyncData data;
            std::string error;
            check(history::parse(hex(protoHex), data, error), "parse plain HistorySync", error);
            check(data.chats.size() == 2, "plain parse chat count");
        } else if (kind == "MEDIA") {
            std::string name, blobHex, keyHex, plainHex;
            ss >> name >> blobHex >> keyHex >> plainHex;

            media::Type type;
            check(media::typeFromName(name, type), ("media type name " + name).c_str());
            Bytes plain;
            std::string error;
            check(media::decryptBlob(hex(blobHex), hex(keyHex), type, plain, error),
                  "media blob decrypt", error);
            check(plain == hex(plainHex), ("media plaintext " + name).c_str());
            check(!media::hkdfInfo(type).empty(), "hkdf info non-empty");
            mediaBlobs++;
        } else if (kind == "CORRUPT") {
            std::string blobHex, keyHex;
            ss >> blobHex >> keyHex;
            Bytes plain;
            std::string error;
            check(!media::decryptBlob(hex(blobHex), hex(keyHex), media::Type::Image, plain, error),
                  "corrupt blob rejected");
            check(error.find("MAC") != std::string::npos, "corruption reported as MAC failure",
                  error);
            // Without MAC verification the padding check still has to fail or
            // produce garbage — decryption must never silently claim success.
            Bytes lax;
            std::string laxErr;
            media::decryptBlob(hex(blobHex), hex(keyHex), media::Type::Image, lax, laxErr, false);
            check(lax != plain || !lax.empty(), "lax path is not identical to verified output");
        }
    }

    // Merged state: history chunks + pushnames, exactly what the UI stores.
    check(historyChunks == 1, "one history chunk");
    check(pushChunks == 1, "one pushname chunk");
    check(merged.chats.size() == 2, "merged chat count");
    check(merged.messageCount() == 3, "merged message count");
    check(merged.progress == 100, "merged progress from last chunk");

    bool groupHasName = false, directHasName = false;
    for (const auto &chat : merged.chats) {
        if (chat.jid == "120363041234567890@g.us" && chat.name == "Family") groupHasName = true;
        if (chat.jid == "919999999999@s.whatsapp.net" && chat.name == "Me") directHasName = true;
    }
    check(groupHasName, "merged group name");
    check(directHasName, "pushname applied to direct chat");

    // HKDF info strings must match MEDIA_HKDF_KEY_MAPPING exactly.
    check(media::hkdfInfo(media::Type::History) == "WhatsApp History Keys", "history info");
    check(media::hkdfInfo(media::Type::ImageThumbnail) == "WhatsApp Image Thumbnail Keys",
          "thumbnail info");
    check(media::hkdfInfo(media::Type::AppState) == "WhatsApp App State Keys", "app state info");

    // Encrypt -> decrypt round-trip through our own implementation.
    {
        const Bytes key = crypto::fromHex(
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        const Bytes plain = {'r', 'o', 'u', 'n', 'd', 't', 'r', 'i', 'p'};
        Bytes blob, back;
        std::string error;
        check(media::encryptBlob(plain, key, media::Type::Document, blob, error),
              "encrypt blob", error);
        check(media::decryptBlob(blob, key, media::Type::Document, back, error),
              "decrypt own blob", error);
        check(back == plain, "encrypt/decrypt round-trip");
        check(media::decryptBlob(blob, key, media::Type::Image, back, error) == false,
              "wrong hkdf type is rejected");
    }

    check(media::urlFromDirectPath("/mms/md-msg-hist/abc") ==
              "https://mmg.whatsapp.net/mms/md-msg-hist/abc",
          "direct path url");

    std::printf("\n=== History / media crypto ===\n");
    std::printf("History chunks  : %d\n", historyChunks);
    std::printf("Pushname chunks : %d\n", pushChunks);
    std::printf("Media blobs     : %d\n", mediaBlobs);
    std::printf("\n%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL HISTORY TESTS PASSED" : "HISTORY TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
