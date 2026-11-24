// Message send/receive plumbing for direct mode — the WAProto + Signal glue.
//
// This is the C++ equivalent of Baileys' lib/Utils/signal.js (bundle parsing),
// lib/Socket/messages-send.js (relayMessage) and
// lib/Utils/decode-wa-message.js (decryptMessageNode), minus the parts that
// need app state:
//
//   outgoing: <iq xmlns=encrypt><key><user jid=…/></key></iq>
//             -> parse per-device pre-key bundles
//             -> Signal session (X3DH) -> encrypt(pad(Message)) -> <enc>
//   incoming: <message><enc type=msg|pkmsg>…</enc></message>
//             -> Signal decrypt -> unpad -> Message.decode -> deviceSent unwrap
//
// Pure C++17 (no Qt) so it is unit-testable against libsignal; WhatsAppDirect
// feeds it BinaryNodes straight off the Noise transport.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <set>

#include "binary_node.h"
#include "sender_key.h"
#include "signal.h"
#include "wa_proto.h"

namespace wa {
namespace messaging {

using Bytes = std::vector<uint8_t>;

// True when both jids belong to the same user (device/id suffix ignored).
bool sameUser(const std::string &a, const std::string &b);

// ── Pre-key bundles ────────────────────────────────────────────────────────

struct DeviceBundle {
    // Note: device 0 is spelled without a suffix on the wire (`u@s.whatsapp.net`),
    // exactly like Baileys' jidEncode(user, server, 0).
    std::string jid;          // "91999…:2@s.whatsapp.net"
    signal::Bundle bundle;
};

// Parses the <iq xmlns="encrypt" type="result"> answer to a key request:
//   <list><user jid="u@s.whatsapp.net"><device jid="u:0@s.whatsapp.net">
//     <key><id/><value/></key> <skey><id/><value/><signature/></skey>
//     <identity/> <registration/>
//   </device></user></list>
bool parseKeyResponse(const BinaryNode &iq, std::vector<DeviceBundle> &out,
                      std::string &error);

// Builds the key request iq. `jids` may be user or device jids; duplicates are
// dropped. The server answers with every device of the requested users.
BinaryNode buildKeyRequest(const std::string &id, const std::vector<std::string> &jids);

// ── Group metadata (iq xmlns="w:g2") ──────────────────────────────────────

struct GroupInfo {
    std::string jid;
    std::string subject;
    std::string addressingMode;              // "pn" | "lid" (may be empty)
    std::vector<std::string> participants;   // member jids (devices fetched separately)
};

// Request: <iq type=get xmlns="w:g2" to=groupJid><query/></iq>
BinaryNode buildGroupMetadataRequest(const std::string &id, const std::string &groupJid);

// Response: <iq type=result><group id subject addressing_mode>
//             <participant jid=…/>…</group></iq>
bool parseGroupMetadata(const BinaryNode &iq, GroupInfo &out, std::string &error);

// ── Outgoing messages ─────────────────────────────────────────────────────

struct SendPlan {
    BinaryNode stanza;        // <message> ready for sendNode()
    bool usedPreKey = false;  // a pkmsg was produced (session was new)
    size_t deviceCount = 0;
};

// Encrypts a Message for every bundle and assembles the <message> stanza.
// Copies addressed to our own devices are wrapped in deviceSentMessage.
bool buildMessageStanza(signal::Store &store, const std::string &meJid,
                        const std::string &chatJid, const waproto::Message &message,
                        const std::string &msgId,
                        const std::vector<DeviceBundle> &bundles, SendPlan &out,
                        std::string &error);

// Encrypts `text` for every bundle and assembles the <message> stanza.
bool buildTextStanza(signal::Store &store, const std::string &meJid,
                     const std::string &chatJid, const std::string &text,
                     const std::string &msgId,
                     const std::vector<DeviceBundle> &bundles, SendPlan &out,
                     std::string &error);

// Group text: encrypts once with our sender key (type "skmsg") and wraps the
// sender-key distribution message in 1:1 encrypted <to> nodes for every device
// that has not received it yet (`alreadyDistributed`, i.e. sender-key-memory).
struct GroupSendPlan {
    BinaryNode stanza;
    Bytes distribution;                     // serialized SKDM that was sent
    std::vector<std::string> distributedTo; // device jids that received it
    size_t deviceCount = 0;
};

bool buildGroupMessageStanza(signal::Store &sessions, signal::SenderKeyStore &senderKeys,
                             const std::string &meJid, const std::string &groupJid,
                             const waproto::Message &message, const std::string &msgId,
                             const std::vector<DeviceBundle> &bundles,
                             const std::set<std::string> &alreadyDistributed,
                             GroupSendPlan &out, std::string &error);

// Text convenience wrapper around buildGroupMessageStanza.
bool buildGroupTextStanza(signal::Store &sessions, signal::SenderKeyStore &senderKeys,
                          const std::string &meJid, const std::string &groupJid,
                          const std::string &text, const std::string &msgId,
                          const std::vector<DeviceBundle> &bundles,
                          const std::set<std::string> &alreadyDistributed,
                          GroupSendPlan &out, std::string &error);

// Encrypts one message payload for one device, returning the <to jid><enc>
// node. A Signal session is built from `device.bundle` when none exists yet
// (Baileys' assertSessions), which is what makes the first message a pkmsg.
bool encryptForDevice(signal::Store &store, const DeviceBundle &device,
                      const waproto::Message &message, bool wrapDeviceSent,
                      const std::string &chatJid, BinaryNode &toNode,
                      bool &usedPreKey, std::string &error);

// ── Incoming messages ─────────────────────────────────────────────────────

struct IncomingMessage {
    std::string chatJid;        // key.remoteJid
    std::string senderJid;      // author of the message (participant in groups)
    std::string recipient;      // key.recipient (our own device)
    std::string id;
    std::string participant;
    std::string pushName;       // stanza attr `notify`
    bool fromMe = false;
    bool isGroup = false;
    uint64_t timestamp = 0;
    waproto::Message message;
    std::string text;

    // Set when the message carried a HistorySyncNotification, so the caller can
    // download + decrypt the chunk (wa_history.h).
    bool hasHistorySync = false;
    waproto::HistorySyncNotification historySync;

    // Set when this message carried a sender-key distribution message that we
    // installed (group messages that follow can then be decrypted).
    bool receivedSenderKey = false;

    // Set when an <enc> payload could not be decrypted (retry receipts).
    bool decrypted = false;
    std::string error;
};

// Signal-encrypted messages that also carry a group sender-key distribution
// message must be handed to a SenderKeyStore; pass nullptr for pure 1:1 use.
// Reads routing attributes only (no decryption), like decodeMessageNode.
bool decodeMessageNode(const BinaryNode &node, const std::string &meJid,
                       IncomingMessage &out, std::string &error);

// decodeMessageNode + Signal decrypt + Message.decode. Handles msg/pkmsg/skmsg
// and installs sender-key distribution messages (needs `senderKeys` for groups).
bool decryptMessageNode(signal::Store &store, signal::SenderKeyStore *senderKeys,
                        const BinaryNode &node, const std::string &meJid,
                        IncomingMessage &out, std::string &error);

// 1:1-only convenience overload (no sender-key support).
inline bool decryptMessageNode(signal::Store &store, const BinaryNode &node,
                               const std::string &meJid, IncomingMessage &out,
                               std::string &error) {
    return decryptMessageNode(store, nullptr, node, meJid, out, error);
}

// ── Helpers ───────────────────────────────────────────────────────────────

// "3EB0" + 16 random hex chars, like Baileys' generateMessageIDV2.
std::string generateMessageId();

} // namespace messaging
} // namespace wa
