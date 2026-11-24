// WAProto subset — the message/history structures a WhatsApp Web client needs.
//
// Baileys generates all of WAProto.proto (9k lines) with protobufjs. Requiring
// protoc on Symbian is painful, so this file hand-writes just the messages on
// the send/receive/history path, using the same tiny protobuf writer as the
// handshake (proto_min):
//
//   Message         — conversation, extended text, image/video/audio/document/
//                     sticker, protocol (history-sync notifications), reaction
//   WebMessageInfo  — key + message + timestamp + pushName (history sync rows)
//   HistorySync     — conversations, embedded messages, pushnames, progress
//   HistorySyncNotification, MessageKey, ContextInfo, media metadata
//
// Field numbers are taken verbatim from WAProto.proto and encoding is verified
// against protobufjs in qt/tests/test_proto.cpp. Everything else in WAProto
// (app state, USync, polls, payments…) is intentionally out of scope for now.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "proto_min.h"

namespace wa {
namespace waproto {

using Bytes = std::vector<uint8_t>;

// ── Shared bits ───────────────────────────────────────────────────────────

struct ContextInfo {
    std::string stanzaId;        // 1
    std::string participant;     // 2
    Bytes quotedMessage;         // 3 (serialized Message)
    std::string remoteJid;       // 4
    uint64_t expiration = 0;     // 6
};

// Media metadata shared by image/video/audio/document/sticker messages.
// Field numbers differ per message type; see WAProto.proto (ImageMessage,
// VideoMessage, AudioMessage, DocumentMessage, StickerMessage).
struct MediaInfo {
    std::string url;             // 1
    std::string mimetype;        // 2 (5 for stickers)
    std::string caption;         // image 3, video 7, document 20
    std::string title;           // document 3
    Bytes fileSha256;
    uint64_t fileLength = 0;
    uint32_t seconds = 0;        // audio/video
    bool ptt = false;            // audio 6
    bool gifPlayback = false;    // video 8
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t pageCount = 0;      // document 6
    Bytes mediaKey;
    Bytes fileEncSha256;
    std::string directPath;
    std::string fileName;        // document 8
    int64_t mediaKeyTimestamp = 0;
    Bytes jpegThumbnail;         // 16
    ContextInfo contextInfo;     // 17
    bool hasContextInfo = false;

    bool empty() const { return url.empty() && mediaKey.empty() && !fileLength; }
};

struct Reaction {
    std::string keyId;           // MessageKey of the target
    std::string text;            // emoji ("" removes)
    std::string senderJid;
};

struct HistorySyncNotification {
    Bytes fileSha256;            // 1
    uint64_t fileLength = 0;     // 2
    Bytes mediaKey;              // 3
    Bytes fileEncSha256;         // 4
    std::string directPath;      // 5
    uint32_t syncType = 0;       // 6
    uint32_t chunkOrder = 0;     // 7
    uint32_t progress = 0;       // 9
    Bytes inlinePayload;         // 11 (initial bootstrap)
};

struct MessageKey {
    std::string remoteJid;       // 1
    bool fromMe = false;         // 2
    std::string id;              // 3
    std::string participant;     // 4
};

// Message.senderKeyDistributionMessage (field 2): the group's jid plus the
// serialized Signal sender-key distribution message (0x33 || proto).
struct SenderKeyDistributionInfo {
    std::string groupId;         // 1
    Bytes axolotl;               // 2
};

// The parts of a Message the client renders.
struct Message {
    std::string conversation;    // 1
    bool hasSenderKeyDistribution = false;
    SenderKeyDistributionInfo senderKeyDistribution;   // 2
    bool hasExtendedText = false;
    std::string extendedText;    // 6.text
    MediaInfo image;             // 3
    MediaInfo video;             // 9
    MediaInfo audio;             // 8
    MediaInfo document;          // 7
    MediaInfo sticker;           // 26
    bool hasProtocolMessage = false;
    MessageKey protocolKey;      // 12.key
    uint32_t protocolType = 0;   // 12.type
    HistorySyncNotification historySync; // 12.historySyncNotification
    bool hasHistorySync = false;
    bool hasReaction = false;
    Reaction reaction;           // 46

    // Sent-by-us copies are wrapped as deviceSentMessage{destinationJid,
    // message} so our other devices can re-deliver them to the right chat.
    bool hasDeviceSent = false;
    std::string deviceSentDestinationJid;   // 31.1
    Bytes deviceSentPayload;                // 31.2 (serialized Message)

    // Highest-level text for list rendering.
    std::string text() const;
    std::string mediaType() const;   // "image" | "video" | ... | ""
};

// ── WebMessageInfo / HistorySync ─────────────────────────────────────────

struct WebMessageInfo {
    MessageKey key;
    Message message;
    bool hasMessage = false;
    uint64_t messageTimestamp = 0;
    std::string participant;
    std::string pushName;
    uint32_t status = 0;
};

struct HistoryConversation {
    std::string id;
    std::string name;
    std::string newJid;
    uint32_t unreadCount = 0;
    uint64_t lastMsgTimestamp = 0;
    uint64_t conversationTimestamp = 0;
    std::vector<WebMessageInfo> messages;
};

struct HistoryPushname {
    std::string id;
    std::string pushname;
};

struct HistorySync {
    uint32_t syncType = 0;
    uint32_t chunkOrder = 0;
    uint32_t progress = 0;
    std::vector<HistoryConversation> conversations;
    std::vector<HistoryPushname> pushnames;
    std::map<std::string, std::string> phoneToLid;   // 15
};

// ── Codec ─────────────────────────────────────────────────────────────────

Bytes encodeMessage(const Message &message);
bool decodeMessage(const Bytes &data, Message &out);

Bytes encodeWebMessageInfo(const WebMessageInfo &info);
bool decodeWebMessageInfo(const Bytes &data, WebMessageInfo &out);

bool decodeHistorySync(const Bytes &data, HistorySync &out);
bool decodeHistorySyncNotification(const Bytes &data, HistorySyncNotification &out);

// ── Pre-key bundle (BinaryNode based, not protobuf) ──────────────────────
// WhatsApp answers <iq><key/></iq> with a <user> stanza carrying the device's
// registration id, identity key and pre-keys.
struct RemoteBundle {
    std::string jid;
    uint32_t registrationId = 0;
    Bytes identityKey;              // 33 bytes (0x05 prefix)
    uint32_t signedPreKeyId = 0;
    Bytes signedPreKeyPublic;       // 33 bytes
    Bytes signedPreKeySignature;    // 64 bytes
    bool hasOneTimePreKey = false;
    uint32_t oneTimePreKeyId = 0;
    Bytes oneTimePreKeyPublic;      // 33 bytes
    std::vector<std::string> deviceJids;
};

// ── Padding (message payloads are PKCS#7-padded to 16 bytes) ─────────────
Bytes padMessage(const Bytes &plaintext, size_t blockSize = 16);
bool unpadMessage(const Bytes &padded, Bytes &plaintext);

} // namespace waproto
} // namespace wa
