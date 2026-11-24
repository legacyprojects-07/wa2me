#include "wa_proto.h"

#include <cstring>

#include "sha256.h"

namespace wa {
namespace waproto {

namespace {

// ── Context + media helpers ───────────────────────────────────────────────

Bytes encodeContextInfo(const ContextInfo &c) {
    proto::Writer w;
    if (!c.stanzaId.empty()) w.fieldString(1, c.stanzaId);
    if (!c.participant.empty()) w.fieldString(2, c.participant);
    if (!c.quotedMessage.empty()) w.fieldBytes(3, c.quotedMessage);
    if (!c.remoteJid.empty()) w.fieldString(4, c.remoteJid);
    if (c.expiration) w.fieldVarint(6, c.expiration);
    return w.data();
}

void decodeContextInfo(const Bytes &data, ContextInfo &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.stanzaId.assign(f.bytes.begin(), f.bytes.end()); break;
        case 2: out.participant.assign(f.bytes.begin(), f.bytes.end()); break;
        case 3: out.quotedMessage = f.bytes; break;
        case 4: out.remoteJid.assign(f.bytes.begin(), f.bytes.end()); break;
        case 6: out.expiration = f.varint; break;
        default: break;
        }
    }
}

// Encodes image/video/audio/document/sticker. `kind` selects the layout
// differences documented in WAProto.proto.
Bytes encodeMedia(const MediaInfo &m, int kind) {
    proto::Writer w;
    if (!m.url.empty()) w.fieldString(1, m.url);
    if (!m.mimetype.empty()) w.fieldString(2, m.mimetype);

    // caption / title position per message type
    if (kind == 3 /*image*/ || kind == 9 /*video*/) {
        if (!m.caption.empty()) w.fieldString(3, m.caption);
        if (!m.fileSha256.empty()) w.fieldBytes(4, m.fileSha256);
        if (m.fileLength) w.fieldVarint(5, m.fileLength);
        if (kind == 3) {
            if (m.mediaKey.size()) w.fieldBytes(8, m.mediaKey);
            if (!m.fileEncSha256.empty()) w.fieldBytes(9, m.fileEncSha256);
            if (!m.directPath.empty()) w.fieldString(11, m.directPath);
        } else {
            if (m.mediaKey.size()) w.fieldBytes(6, m.mediaKey);
            if (!m.caption.empty()) w.fieldString(7, m.caption);
            if (!m.fileEncSha256.empty()) w.fieldBytes(11, m.fileEncSha256);
            if (!m.directPath.empty()) w.fieldString(13, m.directPath);
        }
    } else if (kind == 8 /*audio*/) {
        if (!m.fileSha256.empty()) w.fieldBytes(3, m.fileSha256);
        if (m.fileLength) w.fieldVarint(4, m.fileLength);
        if (m.seconds) w.fieldVarint(5, m.seconds);
        if (m.ptt) w.fieldBool(6, true);
        if (m.mediaKey.size()) w.fieldBytes(7, m.mediaKey);
        if (!m.fileEncSha256.empty()) w.fieldBytes(8, m.fileEncSha256);
        if (!m.directPath.empty()) w.fieldString(9, m.directPath);
    } else if (kind == 7 /*document*/) {
        if (!m.title.empty()) w.fieldString(3, m.title);
        if (!m.fileSha256.empty()) w.fieldBytes(4, m.fileSha256);
        if (m.fileLength) w.fieldVarint(5, m.fileLength);
        if (m.mediaKey.size()) w.fieldBytes(7, m.mediaKey);
        if (!m.fileName.empty()) w.fieldString(8, m.fileName);
        if (!m.fileEncSha256.empty()) w.fieldBytes(9, m.fileEncSha256);
        if (!m.directPath.empty()) w.fieldString(10, m.directPath);
    } else {   // sticker
        if (!m.fileSha256.empty()) w.fieldBytes(2, m.fileSha256);
        if (!m.fileEncSha256.empty()) w.fieldBytes(3, m.fileEncSha256);
        if (m.mediaKey.size()) w.fieldBytes(4, m.mediaKey);
        if (!m.mimetype.empty()) w.fieldString(5, m.mimetype);
        if (!m.directPath.empty()) w.fieldString(8, m.directPath);
        if (m.fileLength) w.fieldVarint(9, m.fileLength);
    }

    if (!m.jpegThumbnail.empty()) w.fieldBytes(16, m.jpegThumbnail);
    if (m.hasContextInfo) w.fieldMessage(17, encodeContextInfo(m.contextInfo));
    return w.data();
}

void decodeMedia(const Bytes &data, int kind, MediaInfo &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.url.assign(f.bytes.begin(), f.bytes.end()); break;
        case 2: out.mimetype.assign(f.bytes.begin(), f.bytes.end()); break;
        case 3:
            if (kind == 7) out.title.assign(f.bytes.begin(), f.bytes.end());
            else out.caption.assign(f.bytes.begin(), f.bytes.end());
            break;
        case 4: out.fileSha256 = f.bytes; break;
        case 5: out.fileLength = f.varint; break;
        case 6:
            if (kind == 8) out.ptt = f.varint != 0;
            else out.mediaKey = f.bytes;
            break;
        case 7:
            if (kind == 7) out.mediaKey = f.bytes;
            else out.caption.assign(f.bytes.begin(), f.bytes.end());
            break;
        case 8:
            if (kind == 7) out.fileName.assign(f.bytes.begin(), f.bytes.end());
            else if (kind == 8) out.fileEncSha256 = f.bytes;
            else out.mediaKey = f.bytes;
            break;
        case 9:
            if (kind == 3) out.fileEncSha256 = f.bytes;
            else out.directPath.assign(f.bytes.begin(), f.bytes.end());
            break;
        case 11:
            if (kind == 3) out.directPath.assign(f.bytes.begin(), f.bytes.end());
            else if (kind == 9) out.fileEncSha256 = f.bytes;
            break;
        case 13: out.directPath.assign(f.bytes.begin(), f.bytes.end()); break;
        case 16: out.jpegThumbnail = f.bytes; break;
        case 17:
            out.hasContextInfo = true;
            decodeContextInfo(f.bytes, out.contextInfo);
            break;
        default: break;
        }
    }
}

void decodeMessageKey(const Bytes &data, MessageKey &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.remoteJid.assign(f.bytes.begin(), f.bytes.end()); break;
        case 2: out.fromMe = f.varint != 0; break;
        case 3: out.id.assign(f.bytes.begin(), f.bytes.end()); break;
        case 4: out.participant.assign(f.bytes.begin(), f.bytes.end()); break;
        default: break;
        }
    }
}

Bytes encodeMessageKey(const MessageKey &k) {
    proto::Writer w;
    if (!k.remoteJid.empty()) w.fieldString(1, k.remoteJid);
    if (k.fromMe) w.fieldBool(2, true);
    if (!k.id.empty()) w.fieldString(3, k.id);
    if (!k.participant.empty()) w.fieldString(4, k.participant);
    return w.data();
}

} // namespace

// ── Message ───────────────────────────────────────────────────────────────

std::string Message::text() const {
    if (!conversation.empty()) return conversation;
    if (hasExtendedText) return extendedText;
    if (hasReaction) return reaction.text;
    for (const MediaInfo *m : {&image, &video, &audio, &document, &sticker}) {
        if (!m->caption.empty()) return m->caption;
        if (!m->fileName.empty()) return m->fileName;
    }
    if (hasProtocolMessage) return "[protocol message]";
    return std::string();
}

std::string Message::mediaType() const {
    if (!image.url.empty() || !image.mediaKey.empty()) return "image";
    if (!video.url.empty() || !video.mediaKey.empty()) return "video";
    if (!audio.url.empty() || !audio.mediaKey.empty()) return "audio";
    if (!document.url.empty() || !document.mediaKey.empty()) return "document";
    if (!sticker.url.empty() || !sticker.mediaKey.empty()) return "sticker";
    return std::string();
}

Bytes encodeMessage(const Message &m) {
    proto::Writer w;
    if (!m.conversation.empty()) w.fieldString(1, m.conversation);
    if (!m.image.url.empty() || !m.image.mediaKey.empty())
        w.fieldMessage(3, encodeMedia(m.image, 3));
    if (!m.document.url.empty() || !m.document.mediaKey.empty())
        w.fieldMessage(7, encodeMedia(m.document, 7));
    if (!m.audio.url.empty() || !m.audio.mediaKey.empty())
        w.fieldMessage(8, encodeMedia(m.audio, 8));
    if (!m.video.url.empty() || !m.video.mediaKey.empty())
        w.fieldMessage(9, encodeMedia(m.video, 9));
    if (m.hasExtendedText) {
        proto::Writer ext;
        ext.fieldString(1, m.extendedText);
        if (m.image.hasContextInfo) ext.fieldMessage(17, encodeContextInfo(m.image.contextInfo));
        w.fieldMessage(6, ext.data());
    }
    if (m.hasProtocolMessage) {
        proto::Writer p;
        p.fieldMessage(1, encodeMessageKey(m.protocolKey));
        p.fieldVarint(2, m.protocolType);
        w.fieldMessage(12, p.data());
    }
    if (!m.sticker.url.empty() || !m.sticker.mediaKey.empty())
        w.fieldMessage(26, encodeMedia(m.sticker, 26));
    if (m.hasDeviceSent) {
        proto::Writer ds;
        if (!m.deviceSentDestinationJid.empty())
            ds.fieldString(1, m.deviceSentDestinationJid);
        if (!m.deviceSentPayload.empty()) ds.fieldBytes(2, m.deviceSentPayload);
        w.fieldMessage(31, ds.data());
    }
    return w.data();
}

bool decodeMessage(const Bytes &data, Message &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1:
            out.conversation.assign(f.bytes.begin(), f.bytes.end());
            break;
        case 3: decodeMedia(f.bytes, 3, out.image); break;
        case 6: {
            proto::Reader ext(f.bytes);
            proto::Field g;
            while (ext.next(g)) {
                if (g.number == 1) {
                    out.hasExtendedText = true;
                    out.extendedText.assign(g.bytes.begin(), g.bytes.end());
                } else if (g.number == 17) {
                    out.image.hasContextInfo = true;
                    decodeContextInfo(g.bytes, out.image.contextInfo);
                }
            }
            break;
        }
        case 7: decodeMedia(f.bytes, 7, out.document); break;
        case 8: decodeMedia(f.bytes, 8, out.audio); break;
        case 9: decodeMedia(f.bytes, 9, out.video); break;
        case 12: {
            out.hasProtocolMessage = true;
            proto::Reader p(f.bytes);
            proto::Field g;
            while (p.next(g)) {
                if (g.number == 1) decodeMessageKey(g.bytes, out.protocolKey);
                else if (g.number == 2) out.protocolType = uint32_t(g.varint);
                else if (g.number == 6) {
                    out.hasHistorySync = true;
                    decodeHistorySyncNotification(g.bytes, out.historySync);
                }
            }
            break;
        }
        case 26: decodeMedia(f.bytes, 26, out.sticker); break;
        case 31: {
            out.hasDeviceSent = true;
            proto::Reader ds(f.bytes);
            proto::Field g;
            while (ds.next(g)) {
                if (g.number == 1)
                    out.deviceSentDestinationJid.assign(g.bytes.begin(), g.bytes.end());
                else if (g.number == 2)
                    out.deviceSentPayload = g.bytes;
            }
            break;
        }
        case 46: {
            out.hasReaction = true;
            proto::Reader rr(f.bytes);
            proto::Field g;
            while (rr.next(g)) {
                if (g.number == 1) decodeMessageKey(g.bytes, out.protocolKey);
                else if (g.number == 2) out.reaction.text.assign(g.bytes.begin(), g.bytes.end());
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

// ── WebMessageInfo ────────────────────────────────────────────────────────

Bytes encodeWebMessageInfo(const WebMessageInfo &info) {
    proto::Writer w;
    w.fieldMessage(1, encodeMessageKey(info.key));
    if (info.hasMessage) w.fieldMessage(2, encodeMessage(info.message));
    if (info.messageTimestamp) w.fieldVarint(3, info.messageTimestamp);
    if (info.status) w.fieldVarint(4, info.status);
    if (!info.participant.empty()) w.fieldString(5, info.participant);
    if (!info.pushName.empty()) w.fieldString(19, info.pushName);
    return w.data();
}

bool decodeWebMessageInfo(const Bytes &data, WebMessageInfo &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: decodeMessageKey(f.bytes, out.key); break;
        case 2: out.hasMessage = true; decodeMessage(f.bytes, out.message); break;
        case 3: out.messageTimestamp = f.varint; break;
        case 4: out.status = uint32_t(f.varint); break;
        case 5: out.participant.assign(f.bytes.begin(), f.bytes.end()); break;
        case 19: out.pushName.assign(f.bytes.begin(), f.bytes.end()); break;
        default: break;
        }
    }
    return true;
}

// ── HistorySync ───────────────────────────────────────────────────────────

bool decodeHistorySyncNotification(const Bytes &data, HistorySyncNotification &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.fileSha256 = f.bytes; break;
        case 2: out.fileLength = f.varint; break;
        case 3: out.mediaKey = f.bytes; break;
        case 4: out.fileEncSha256 = f.bytes; break;
        case 5: out.directPath.assign(f.bytes.begin(), f.bytes.end()); break;
        case 6: out.syncType = uint32_t(f.varint); break;
        case 7: out.chunkOrder = uint32_t(f.varint); break;
        case 9: out.progress = uint32_t(f.varint); break;
        case 11: out.inlinePayload = f.bytes; break;
        default: break;
        }
    }
    return true;
}

bool decodeHistorySync(const Bytes &data, HistorySync &out) {
    proto::Reader r(data);
    proto::Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: out.syncType = uint32_t(f.varint); break;
        case 2: {   // Conversation
            HistoryConversation conv;
            proto::Reader cr(f.bytes);
            proto::Field g;
            while (cr.next(g)) {
                switch (g.number) {
                case 1: conv.id.assign(g.bytes.begin(), g.bytes.end()); break;
                case 2: {   // HistorySyncMsg { message = 1 }
                    proto::Reader mr(g.bytes);
                    proto::Field h;
                    while (mr.next(h)) {
                        if (h.number == 1) {
                            WebMessageInfo info;
                            decodeWebMessageInfo(h.bytes, info);
                            conv.messages.push_back(std::move(info));
                        }
                    }
                    break;
                }
                case 3: conv.newJid.assign(g.bytes.begin(), g.bytes.end()); break;
                case 5: conv.lastMsgTimestamp = g.varint; break;
                case 6: conv.unreadCount = uint32_t(g.varint); break;
                case 12: conv.conversationTimestamp = g.varint; break;
                case 13: conv.name.assign(g.bytes.begin(), g.bytes.end()); break;
                default: break;
                }
            }
            out.conversations.push_back(std::move(conv));
            break;
        }
        case 5: out.chunkOrder = uint32_t(f.varint); break;
        case 6: out.progress = uint32_t(f.varint); break;
        case 7: {   // Pushname { id = 1, pushname = 2 }
            HistoryPushname pn;
            proto::Reader pr(f.bytes);
            proto::Field g;
            while (pr.next(g)) {
                if (g.number == 1) pn.id.assign(g.bytes.begin(), g.bytes.end());
                else if (g.number == 2) pn.pushname.assign(g.bytes.begin(), g.bytes.end());
            }
            out.pushnames.push_back(std::move(pn));
            break;
        }
        case 15: {   // PhoneNumberToLIDMapping { pnJid = 1, lidJid = 2 }
            std::string pn, lid;
            proto::Reader pr(f.bytes);
            proto::Field g;
            while (pr.next(g)) {
                if (g.number == 1) pn.assign(g.bytes.begin(), g.bytes.end());
                else if (g.number == 2) lid.assign(g.bytes.begin(), g.bytes.end());
            }
            if (!pn.empty() && !lid.empty()) out.phoneToLid[pn] = lid;
            break;
        }
        default:
            break;
        }
    }
    return true;
}

// ── Padding ───────────────────────────────────────────────────────────────

Bytes padMessage(const Bytes &plaintext, size_t blockSize) {
    // Baileys' writeRandomPadMax16: a random 1..blockSize byte tail whose
    // bytes all equal the pad length. The result is *not* block aligned —
    // Signal's AES-CBC re-pads underneath — so callers must not assume it.
    const uint8_t r = crypto::randomBytes(1).at(0);
    const size_t padLen = (r % blockSize) + 1;
    Bytes out = plaintext;
    out.insert(out.end(), padLen, uint8_t(padLen));
    return out;
}

bool unpadMessage(const Bytes &padded, Bytes &plaintext) {
    // Mirrors unpadRandomMax16: only the length byte is meaningful, because
    // WhatsApp clients fill the tail with random bytes.
    if (padded.empty()) return false;
    const uint8_t padLen = padded.back();
    if (padLen > padded.size()) return false;
    plaintext.assign(padded.begin(), padded.end() - padLen);
    return true;
}

} // namespace waproto
} // namespace wa
