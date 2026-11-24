// HistorySync decoding — decrypt, inflate and flatten WhatsApp's history blobs.
//
// Baileys' lib/Utils/history.js does exactly this:
//   downloadContentFromMessage(msg, 'md-msg-hist')   // AES-CBC + MAC, see media_crypto
//   -> zlib.inflate()                                // see inflateZlib
//   -> proto.HistorySync.decode()
//   -> processHistoryMessage(): chats + contacts + messages
//
// The Qt client uses this for the initial bootstrap (and on-demand chunks),
// which is the "load chat history" half of direct mode.

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "wa_proto.h"

namespace wa {
namespace history {

using Bytes = std::vector<uint8_t>;

struct Chat {
    std::string jid;                       // HistoryConversation.id
    std::string name;                      // group subject / contact name (may be empty)
    uint32_t unreadCount;
    uint64_t lastMessageTimestamp;  // conversationTimestamp
    std::vector<waproto::WebMessageInfo> messages;   // as delivered (newest first)
    Chat() : unreadCount(0), lastMessageTimestamp(0) {}
};

struct Contact {
    std::string jid;
    std::string pushname;
};

struct SyncData {
    uint32_t syncType;
    uint32_t chunkOrder;
    uint32_t progress;  // 0..100

    std::vector<Chat> chats;
    std::vector<Contact> contacts;

    // Merge another chunk (chunks arrive out of order, each carrying whole
    // conversations; later chunks win for unread counts / names).
    void merge(const SyncData &other);

    size_t messageCount() const;
    SyncData() : syncType(0), chunkOrder(0), progress(0) {}
};

// Decrypts the blob fetched from notification.directPath and inflates it,
// leaving the serialized HistorySync proto in `historySyncBytes`.
bool decryptPayload(const Bytes &encryptedBlob, const Bytes &mediaKey,
                    Bytes &historySyncBytes, std::string &error);

// Parses a decrypted+inflated HistorySync and flattens it like
// processHistoryMessage (chats, contacts, messages).
bool parse(const Bytes &historySyncBytes, SyncData &out, std::string &error);

// decryptPayload + parse in one call.
bool decode(const Bytes &encryptedBlob, const Bytes &mediaKey, SyncData &out,
            std::string &error);

} // namespace history
} // namespace wa
