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

#include "binary_node.h"
#include "signal.h"
#include "wa_proto.h"

namespace wa {
namespace messaging {

using Bytes = std::vector<uint8_t>;

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

// ── Outgoing messages ─────────────────────────────────────────────────────

struct SendPlan {
    BinaryNode stanza;        // <message> ready for sendNode()
    bool usedPreKey = false;  // a pkmsg was produced (session was new)
    size_t deviceCount = 0;
};

// Encrypts `text` for every bundle and assembles the <message> stanza.
// Copies addressed to our own devices are wrapped in deviceSentMessage.
bool buildTextStanza(signal::Store &store, const std::string &meJid,
                     const std::string &chatJid, const std::string &text,
                     const std::string &msgId,
                     const std::vector<DeviceBundle> &bundles, SendPlan &out,
                     std::string &error);

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

    // Set when an <enc> payload could not be decrypted (retry receipts).
    bool decrypted = false;
    std::string error;
};

// Reads routing attributes only (no decryption), like decodeMessageNode.
bool decodeMessageNode(const BinaryNode &node, const std::string &meJid,
                       IncomingMessage &out, std::string &error);

// decodeMessageNode + Signal decrypt + Message.decode.
bool decryptMessageNode(signal::Store &store, const BinaryNode &node,
                        const std::string &meJid, IncomingMessage &out,
                        std::string &error);

// ── Helpers ───────────────────────────────────────────────────────────────

// "3EB0" + 16 random hex chars, like Baileys' generateMessageIDV2.
std::string generateMessageId();

} // namespace messaging
} // namespace wa
