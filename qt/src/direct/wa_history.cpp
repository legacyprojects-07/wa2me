#include "wa_history.h"

#include <algorithm>
#include <map>

#include "inflate.h"
#include "media_crypto.h"

namespace wa {
namespace history {

bool decryptPayload(const Bytes &encryptedBlob, const Bytes &mediaKey,
                    Bytes &historySyncBytes, std::string &error) {
    historySyncBytes.clear();
    Bytes plaintext;
    if (!media::decryptBlob(encryptedBlob, mediaKey, media::Type::History, plaintext,
                            error)) {
        return false;
    }
    if (!binary::inflateZlib(plaintext, historySyncBytes)) {
        error = "history payload is not zlib-compressed (inflate failed)";
        historySyncBytes.clear();
        return false;
    }
    return true;
}

bool parse(const Bytes &historySyncBytes, SyncData &out, std::string &error) {
    out = SyncData();
    waproto::HistorySync sync;
    if (!waproto::decodeHistorySync(historySyncBytes, sync)) {
        error = "HistorySync protobuf did not parse";
        return false;
    }
    out.syncType = sync.syncType;
    out.chunkOrder = sync.chunkOrder;
    out.progress = sync.progress;

    for (const auto &conv : sync.conversations) {
        Chat chat;
        chat.jid = conv.id;
        chat.name = conv.name;
        chat.unreadCount = conv.unreadCount;
        chat.lastMessageTimestamp = conv.conversationTimestamp;
        chat.messages = conv.messages;
        out.chats.push_back(chat);

        // Baileys also registers every conversation as a contact.
        Contact c;
        c.jid = conv.id;
        c.pushname = conv.name;
        if (!c.jid.empty()) out.contacts.push_back(c);
    }

    // PUSH_NAME chunks carry the address-book names.
    for (const auto &pn : sync.pushnames) {
        if (pn.id.empty()) continue;
        bool merged = false;
        for (auto &c : out.contacts) {
            if (c.jid == pn.id) {
                c.pushname = pn.pushname;
                merged = true;
                break;
            }
        }
        if (!merged) out.contacts.push_back(Contact{pn.id, pn.pushname});
    }

    // Chat names for direct chats come from the pushname list.
    for (auto &chat : out.chats) {
        if (!chat.name.empty()) continue;
        for (const auto &c : out.contacts) {
            if (c.jid == chat.jid) {
                chat.name = c.pushname;
                break;
            }
        }
    }
    return true;
}

bool decode(const Bytes &encryptedBlob, const Bytes &mediaKey, SyncData &out,
            std::string &error) {
    Bytes proto;
    if (!decryptPayload(encryptedBlob, mediaKey, proto, error)) return false;
    return parse(proto, out, error);
}

void SyncData::merge(const SyncData &other) {
    if (other.progress) progress = other.progress;
    if (other.syncType) syncType = other.syncType;
    if (other.chunkOrder) chunkOrder = other.chunkOrder;

    for (const auto &incoming : other.chats) {
        auto it = std::find_if(chats.begin(), chats.end(),
                               [&](const Chat &c) { return c.jid == incoming.jid; });
        if (it == chats.end()) {
            chats.push_back(incoming);
            continue;
        }
        if (!incoming.name.empty()) it->name = incoming.name;
        if (incoming.unreadCount) it->unreadCount = incoming.unreadCount;
        if (incoming.lastMessageTimestamp)
            it->lastMessageTimestamp = incoming.lastMessageTimestamp;
        // Append the chunk's messages, skipping ids we already have.
        for (const auto &m : incoming.messages) {
            const bool dup = std::any_of(it->messages.begin(), it->messages.end(),
                                         [&](const waproto::WebMessageInfo &have) {
                                             return have.key.id == m.key.id;
                                         });
            if (!dup) it->messages.push_back(m);
        }
    }

    for (const auto &c : other.contacts) {
        auto it = std::find_if(contacts.begin(), contacts.end(),
                               [&](const Contact &have) { return have.jid == c.jid; });
        if (it == contacts.end())
            contacts.push_back(c);
        else if (!c.pushname.empty())
            it->pushname = c.pushname;
    }

    // PUSH_NAME chunks arrive after the conversations, so re-apply contact
    // names to any chat that is still nameless.
    for (auto &chat : chats) {
        if (!chat.name.empty()) continue;
        for (const auto &c : contacts) {
            if (c.jid == chat.jid) {
                chat.name = c.pushname;
                break;
            }
        }
    }
}

size_t SyncData::messageCount() const {
    size_t n = 0;
    for (const auto &c : chats) n += c.messages.size();
    return n;
}

} // namespace history
} // namespace wa
