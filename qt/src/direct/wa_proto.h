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
    uint64_t expiration;  // 6
    ContextInfo() : expiration(0) {}
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
    uint64_t fileLength;
    uint32_t seconds;  // audio/video
    bool ptt;  // audio 6
    bool gifPlayback;  // video 8
    uint32_t height;
    uint32_t width;
    uint32_t pageCount;  // document 6
    Bytes mediaKey;
    Bytes fileEncSha256;
    std::string directPath;
    std::string fileName;        // document 8
    int64_t mediaKeyTimestamp;
    Bytes jpegThumbnail;         // 16
    ContextInfo contextInfo;     // 17
    bool hasContextInfo;

    bool empty() const { return url.empty() && mediaKey.empty() && !fileLength; }
    MediaInfo()
        : fileLength(0),
          seconds(0),
          ptt(false),
          gifPlayback(false),
          height(0),
          width(0),
          pageCount(0),
          mediaKeyTimestamp(0),
          hasContextInfo(false) {}
};

struct Reaction {
    std::string keyId;           // MessageKey of the target
    std::string text;            // emoji ("" removes)
    std::string senderJid;
};

struct HistorySyncNotification {
    Bytes fileSha256;            // 1
    uint64_t fileLength;  // 2
    Bytes mediaKey;              // 3
    Bytes fileEncSha256;         // 4
    std::string directPath;      // 5
    uint32_t syncType;  // 6
    uint32_t chunkOrder;  // 7
    uint32_t progress;  // 9
    Bytes inlinePayload;         // 11 (initial bootstrap)
    HistorySyncNotification() : fileLength(0), syncType(0), chunkOrder(0), progress(0) {}
};

struct MessageKey {
    std::string remoteJid;       // 1
    bool fromMe;  // 2
    std::string id;              // 3
    std::string participant;     // 4
    MessageKey() : fromMe(false) {}
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
    bool hasSenderKeyDistribution;
    SenderKeyDistributionInfo senderKeyDistribution;   // 2
    bool hasExtendedText;
    std::string extendedText;    // 6.text
    MediaInfo image;             // 3
    MediaInfo video;             // 9
    MediaInfo audio;             // 8
    MediaInfo document;          // 7
    MediaInfo sticker;           // 26
    bool hasProtocolMessage;
    MessageKey protocolKey;      // 12.key
    uint32_t protocolType;  // 12.type
    HistorySyncNotification historySync; // 12.historySyncNotification
    bool hasHistorySync;
    bool hasReaction;
    Reaction reaction;           // 46

    // Sent-by-us copies are wrapped as deviceSentMessage{destinationJid,
    // message} so our other devices can re-deliver them to the right chat.
    bool hasDeviceSent;
    std::string deviceSentDestinationJid;   // 31.1
    Bytes deviceSentPayload;                // 31.2 (serialized Message)

    // Highest-level text for list rendering.
    std::string text() const;
    std::string mediaType() const;   // "image" | "video" | ... | ""
    Message()
        : hasSenderKeyDistribution(false),
          hasExtendedText(false),
          hasProtocolMessage(false),
          protocolType(0),
          hasHistorySync(false),
          hasReaction(false),
          hasDeviceSent(false) {}
};

// ── WebMessageInfo / HistorySync ─────────────────────────────────────────

struct WebMessageInfo {
    MessageKey key;
    Message message;
    bool hasMessage;
    uint64_t messageTimestamp;
    std::string participant;
    std::string pushName;
    uint32_t status;
    WebMessageInfo() : hasMessage(false), messageTimestamp(0), status(0) {}
};

struct HistoryConversation {
    std::string id;
    std::string name;
    std::string newJid;
    uint32_t unreadCount;
    uint64_t lastMsgTimestamp;
    uint64_t conversationTimestamp;
    std::vector<WebMessageInfo> messages;
    HistoryConversation() : unreadCount(0), lastMsgTimestamp(0), conversationTimestamp(0) {}
};

struct HistoryPushname {
    std::string id;
    std::string pushname;
};

struct HistorySync {
    uint32_t syncType;
    uint32_t chunkOrder;
    uint32_t progress;
    std::vector<HistoryConversation> conversations;
    std::vector<HistoryPushname> pushnames;
    std::map<std::string, std::string> phoneToLid;   // 15
    HistorySync() : syncType(0), chunkOrder(0), progress(0) {}
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
    uint32_t registrationId;
    Bytes identityKey;              // 33 bytes (0x05 prefix)
    uint32_t signedPreKeyId;
    Bytes signedPreKeyPublic;       // 33 bytes
    Bytes signedPreKeySignature;    // 64 bytes
    bool hasOneTimePreKey;
    uint32_t oneTimePreKeyId;
    Bytes oneTimePreKeyPublic;      // 33 bytes
    std::vector<std::string> deviceJids;
    RemoteBundle()
        : registrationId(0),
          signedPreKeyId(0),
          hasOneTimePreKey(false),
          oneTimePreKeyId(0) {}
};

// ── Padding (message payloads are PKCS#7-padded to 16 bytes) ─────────────
Bytes padMessage(const Bytes &plaintext, size_t blockSize = 16);
bool unpadMessage(const Bytes &padded, Bytes &plaintext);

} // namespace waproto
} // namespace wa
