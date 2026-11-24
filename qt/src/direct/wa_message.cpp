#include "wa_message.h"

#include <algorithm>
#include <cstdio>

#include "sender_key.h"
#include "sha256.h"

namespace wa {
namespace messaging {

namespace {

// WhatsApp wraps raw curve25519 public keys in a 33-byte blob starting with
// 0x05 (Baileys' generateSignalPubKey).
Bytes signalPubKey(const Bytes &raw) {
    if (raw.size() == 33) return raw;
    Bytes out;
    out.reserve(raw.size() + 1);
    out.push_back(0x05);
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

bool nodeBytes(const BinaryNode *node, Bytes &out) {
    if (!node || node->contentType != BinaryNode::Content::Bytes) return false;
    out = node->bytes;
    return true;
}

// Big-endian unsigned integer from a node's byte content (xmppPreKey ids).
bool nodeUInt(const BinaryNode *node, size_t maxBytes, uint32_t &out) {
    Bytes b;
    if (!nodeBytes(node, b)) return false;
    if (b.size() > maxBytes) b.erase(b.begin(), b.end() - maxBytes);
    uint32_t v = 0;
    for (uint8_t c : b) v = (v << 8) | c;
    out = v;
    return true;
}

std::string userPart(const std::string &jid) {
    Jid decoded = jidDecode(jid);
    return decoded.valid ? decoded.user : jid;
}

} // namespace

bool sameUser(const std::string &a, const std::string &b) {
    return !a.empty() && !b.empty() && userPart(a) == userPart(b);
}

// ── Pre-key bundles ───────────────────────────────────────────────────────

bool parseKeyResponse(const BinaryNode &iq, std::vector<DeviceBundle> &out,
                      std::string &error) {
    out.clear();
    if (const BinaryNode *err = iq.child("error")) {
        error = "key query failed: " + err->attrOr("text", err->attrOr("code", "unknown"));
        return false;
    }
    const BinaryNode *list = iq.child("list");
    if (!list) {
        error = "key response has no <list>";
        return false;
    }
    for (const BinaryNode *user : list->childrenNamed("user")) {
        const std::string userJid = user->attrOr("jid");
        if (const BinaryNode *err = user->child("error")) {
            // One dead user must not abort the whole batch.
            (void)err;
            continue;
        }
        for (const BinaryNode *device : user->childrenNamed("device")) {
            DeviceBundle db;
            db.jid = device->attrOr("jid", userJid);
            if (db.jid.empty()) continue;

            Bytes identity;
            if (!nodeBytes(device->child("identity"), identity) || identity.empty()) continue;
            db.bundle.identityKey = signalPubKey(identity);
            nodeUInt(device->child("registration"), 4, db.bundle.registrationId);

            if (const BinaryNode *skey = device->child("skey")) {
                nodeUInt(skey->child("id"), 3, db.bundle.signedPreKey.id);
                Bytes value;
                if (!nodeBytes(skey->child("value"), value)) continue;
                db.bundle.signedPreKey.pair.pub = signalPubKey(value);
                nodeBytes(skey->child("signature"), db.bundle.signedPreKey.signature);
                if (db.bundle.signedPreKey.pair.pub.size() != 33) continue;
            } else {
                continue;   // a device without a signed pre-key is unusable
            }

            if (const BinaryNode *key = device->child("key")) {
                Bytes value;
                if (nodeBytes(key->child("value"), value) && !value.empty()) {
                    nodeUInt(key->child("id"), 3, db.bundle.preKey.id);
                    db.bundle.preKey.pair.pub = signalPubKey(value);
                    db.bundle.hasPreKey = db.bundle.preKey.pair.pub.size() == 33;
                }
            }
            out.push_back(db);
        }
    }
    if (out.empty()) {
        error = "key response contained no usable devices";
        return false;
    }
    return true;
}

BinaryNode buildKeyRequest(const std::string &id, const std::vector<std::string> &jids) {
    BinaryNode iq("iq");
    iq.setAttr("id", id);
    iq.setAttr("type", "get");
    iq.setAttr("xmlns", "encrypt");
    iq.setAttr("to", "s.whatsapp.net");

    BinaryNode key("key");
    std::vector<std::string> seen;
    for (const auto &raw : jids) {
        const std::string user = userPart(raw);
        if (user.empty()) continue;
        if (std::find(seen.begin(), seen.end(), user) != seen.end()) continue;
        seen.push_back(user);

        BinaryNode userNode("user");
        userNode.setAttr("jid", jidEncode(user, "s.whatsapp.net"));
        key.addChild(userNode);
    }
    iq.addChild(key);
    return iq;
}

// ── Outgoing messages ─────────────────────────────────────────────────────

bool encryptForDevice(signal::Store &store, const DeviceBundle &device,
                      const waproto::Message &message, bool wrapDeviceSent,
                      const std::string &chatJid, BinaryNode &toNode,
                      bool &usedPreKey, std::string &error) {
    const std::string &deviceJid = device.jid;
    const std::string address = jidNormalizedUser(deviceJid);

    waproto::Message outgoing = message;
    if (wrapDeviceSent) {
        outgoing = waproto::Message();
        outgoing.hasDeviceSent = true;
        outgoing.deviceSentDestinationJid = chatJid;
        outgoing.deviceSentPayload = waproto::encodeMessage(message);
    }

    // Sessions are lazy, exactly like Baileys' assertSessions: build one from
    // the pre-key bundle only when we have no open session for this device.
    {
        signal::Record record = store.loadSession(address);
        if (!record.openSession()) {
            if (!signal::initOutgoingSession(store, address, device.bundle, error)) {
                error = "cannot open session with " + deviceJid + ": " + error;
                return false;
            }
        }
    }

    const Bytes plaintext = waproto::padMessage(waproto::encodeMessage(outgoing));
    const signal::EncryptResult encrypted =
        signal::encrypt(store, address, plaintext);
    if (!encrypted.ok) {
        error = "signal encrypt failed for " + deviceJid + ": " + encrypted.error;
        return false;
    }

    BinaryNode enc("enc");
    enc.setAttr("v", "2");
    // Baileys names the enc types by their E2E "type" string.
    const char *typeName = encrypted.type == 3 ? "pkmsg" : "msg";
    enc.setAttr("type", typeName);
    enc.setBytes(encrypted.body);

    toNode = BinaryNode("to");
    toNode.setAttr("jid", deviceJid);
    toNode.addChild(enc);
    usedPreKey = encrypted.type == 3;
    return true;
}

bool buildMessageStanza(signal::Store &store, const std::string &meJid,
                        const std::string &chatJid, const waproto::Message &message,
                        const std::string &msgId,
                        const std::vector<DeviceBundle> &bundles, SendPlan &out,
                        std::string &error) {
    out = SendPlan();
    if (bundles.empty()) {
        error = "no device bundles to encrypt for";
        return false;
    }
    if (chatJid.empty()) {
        error = "empty chat jid";
        return false;
    }

    BinaryNode participants("participants");
    for (const auto &bundle : bundles) {
        // Our own devices get the deviceSentMessage wrapper so they can route
        // the message to the right chat; the peer gets the bare message.
        const bool isSelf = sameUser(bundle.jid, meJid);

        BinaryNode toNode;
        bool usedPreKey = false;
        if (!encryptForDevice(store, bundle, message, isSelf, chatJid, toNode,
                              usedPreKey, error)) {
            return false;
        }
        out.usedPreKey = out.usedPreKey || usedPreKey;
        participants.addChild(toNode);
        out.deviceCount++;
    }

    out.stanza = BinaryNode("message");
    out.stanza.setAttr("id", msgId);
    // Media messages are sent with type "media" (Baileys uses the chat type).
    out.stanza.setAttr("type", message.mediaType().empty() ? "text" : "media");
    out.stanza.setAttr("to", chatJid);
    out.stanza.addChild(participants);
    return true;
}

bool buildTextStanza(signal::Store &store, const std::string &meJid,
                     const std::string &chatJid, const std::string &text,
                     const std::string &msgId,
                     const std::vector<DeviceBundle> &bundles, SendPlan &out,
                     std::string &error) {
    waproto::Message message;
    if (!text.empty()) message.conversation = text;
    return buildMessageStanza(store, meJid, chatJid, message, msgId, bundles, out, error);
}

// ── Group metadata (w:g2) ─────────────────────────────────────────────────

BinaryNode buildGroupMetadataRequest(const std::string &id, const std::string &groupJid) {
    BinaryNode iq("iq");
    iq.setAttr("id", id);
    iq.setAttr("type", "get");
    iq.setAttr("xmlns", "w:g2");
    iq.setAttr("to", groupJid);
    // Baileys asks for the interactive metadata (request="interactive").
    BinaryNode query("query");
    query.setAttr("request", "interactive");
    iq.addChild(query);
    return iq;
}

bool parseGroupMetadata(const BinaryNode &iq, GroupInfo &out, std::string &error) {
    out = GroupInfo();
    if (const BinaryNode *err = iq.child("error")) {
        error = "group metadata query failed: " + err->attrOr("text", err->attrOr("code", "?"));
        return false;
    }
    const BinaryNode *group = iq.child("group");
    if (!group) {
        error = "group metadata response has no <group>";
        return false;
    }
    out.jid = group->attrOr("id");
    out.subject = group->attrOr("subject");
    out.addressingMode = group->attrOr("addressing_mode");
    if (const BinaryNode *desc = group->child("description")) {
        out.descId = desc->attrOr("id");
        if (const BinaryNode *body = desc->child("body")) out.description = body->text;
    }
    for (const BinaryNode *p : group->childrenNamed("participant")) {
        const std::string jid = p->attrOr("jid");
        if (!jid.empty()) out.participants.push_back(jid);
    }
    if (out.participants.empty()) {
        error = "group has no participants";
        return false;
    }
    return true;
}

// ── Group administration ──────────────────────────────────────────────────

namespace {

BinaryNode groupIq(const std::string &id, const std::string &groupJid,
                   const std::string &type) {
    BinaryNode iq("iq");
    iq.setAttr("id", id);
    iq.setAttr("type", type);
    iq.setAttr("xmlns", "w:g2");
    iq.setAttr("to", groupJid);
    return iq;
}

std::string iqErrorText(const BinaryNode &iq) {
    const BinaryNode *err = iq.child("error");
    if (!err) return std::string();
    std::string text = err->attrOr("text");
    if (text.empty()) text = err->attrOr("code", "?");
    return text;
}

} // namespace

const char *groupActionTag(GroupAction action) {
    switch (action) {
    case GroupAction::Add: return "add";
    case GroupAction::Remove: return "remove";
    case GroupAction::Promote: return "promote";
    case GroupAction::Demote: return "demote";
    }
    return "add";
}

bool parseGroupAction(const std::string &name, GroupAction &out) {
    if (name == "add") { out = GroupAction::Add; return true; }
    if (name == "remove") { out = GroupAction::Remove; return true; }
    if (name == "promote") { out = GroupAction::Promote; return true; }
    if (name == "demote") { out = GroupAction::Demote; return true; }
    return false;
}

BinaryNode buildGroupCreateRequest(const std::string &id, const std::string &subject,
                                   const std::vector<std::string> &participants,
                                   const std::string &key) {
    BinaryNode iq = groupIq(id, "@g.us", "set");
    BinaryNode create("create");
    create.setAttr("subject", subject);
    if (!key.empty()) create.setAttr("key", key);
    for (const std::string &jid : participants) {
        if (jid.empty()) continue;
        BinaryNode p("participant");
        p.setAttr("jid", jid);
        create.addChild(p);
    }
    // <create> always carries the list content (even when empty).
    if (create.contentType != BinaryNode::Content::List) create.setList({});
    iq.addChild(create);
    return iq;
}

BinaryNode buildGroupLeaveRequest(const std::string &id, const std::string &groupJid) {
    BinaryNode iq = groupIq(id, "@g.us", "set");
    BinaryNode leave("leave");
    BinaryNode group("group");
    group.setAttr("id", groupJid);
    leave.addChild(group);
    iq.addChild(leave);
    return iq;
}

BinaryNode buildGroupSubjectRequest(const std::string &id, const std::string &groupJid,
                                    const std::string &subject) {
    BinaryNode iq = groupIq(id, groupJid, "set");
    BinaryNode node("subject");
    node.setText(subject);
    iq.addChild(node);
    return iq;
}

BinaryNode buildGroupDescriptionRequest(const std::string &id, const std::string &groupJid,
                                        const std::string &description,
                                        const std::string &prevId,
                                        const std::string &newDescriptionId) {
    BinaryNode iq = groupIq(id, groupJid, "set");
    BinaryNode node("description");
    if (description.empty()) {
        node.setAttr("delete", "true");
    } else if (!newDescriptionId.empty()) {
        node.setAttr("id", newDescriptionId);
    }
    if (!prevId.empty()) node.setAttr("prev", prevId);
    if (!description.empty()) {
        BinaryNode body("body");
        body.setText(description);
        node.addChild(body);
    }
    iq.addChild(node);
    return iq;
}

BinaryNode buildGroupParticipantsRequest(const std::string &id, const std::string &groupJid,
                                         GroupAction action,
                                         const std::vector<std::string> &participants) {
    BinaryNode iq = groupIq(id, groupJid, "set");
    BinaryNode node(groupActionTag(action));
    for (const std::string &jid : participants) {
        if (jid.empty()) continue;
        BinaryNode p("participant");
        p.setAttr("jid", jid);
        node.addChild(p);
    }
    if (node.contentType != BinaryNode::Content::List) node.setList({});
    iq.addChild(node);
    return iq;
}

BinaryNode buildGroupInviteRequest(const std::string &id, const std::string &groupJid,
                                   bool revoke) {
    BinaryNode iq = groupIq(id, groupJid, revoke ? "set" : "get");
    iq.addChild(BinaryNode("invite"));
    return iq;
}

BinaryNode buildGroupAcceptInviteRequest(const std::string &id, const std::string &code) {
    BinaryNode iq = groupIq(id, "@g.us", "set");
    BinaryNode invite("invite");
    invite.setAttr("code", code);
    iq.addChild(invite);
    return iq;
}

bool parseGroupParticipantsResult(const BinaryNode &iq, GroupAction action,
                                  std::vector<ParticipantChange> &out, std::string &error) {
    out.clear();
    const std::string err = iqErrorText(iq);
    if (!err.empty()) {
        error = std::string("group ") + groupActionTag(action) + " failed: " + err;
        return false;
    }
    const BinaryNode *node = iq.child(groupActionTag(action));
    if (!node) {
        error = std::string("group ") + groupActionTag(action) + " reply has no <" +
                groupActionTag(action) + ">";
        return false;
    }
    for (const BinaryNode *p : node->childrenNamed("participant")) {
        ParticipantChange change;
        change.jid = p->attrOr("jid");
        change.status = p->attrOr("error", "200");   // Baileys: error || '200'
        out.push_back(change);
    }
    return true;
}

bool parseGroupCreated(const BinaryNode &iq, GroupInfo &out, std::string &error) {
    out = GroupInfo();
    const std::string err = iqErrorText(iq);
    if (err.empty()) {
        const BinaryNode *group = iq.child("group");
        if (!group) {
            error = "group create reply has no <group>";
            return false;
        }
        out.jid = group->attrOr("id");
        out.subject = group->attrOr("subject");
        out.addressingMode = group->attrOr("addressing_mode");
        if (const BinaryNode *desc = group->child("description")) {
            out.descId = desc->attrOr("id");
            if (const BinaryNode *body = desc->child("body")) out.description = body->text;
        }
        for (const BinaryNode *p : group->childrenNamed("participant")) {
            const std::string jid = p->attrOr("jid");
            if (!jid.empty()) out.participants.push_back(jid);
        }
        if (out.jid.empty()) {
            error = "group create reply has no id";
            return false;
        }
        return true;
    }
    error = "group create failed: " + err;
    return false;
}

bool parseGroupInviteCode(const BinaryNode &iq, std::string &code, std::string &error) {
    code.clear();
    const std::string err = iqErrorText(iq);
    if (!err.empty()) {
        error = "group invite failed: " + err;
        return false;
    }
    const BinaryNode *invite = iq.child("invite");
    if (!invite || invite->attrOr("code").empty()) {
        error = "group invite reply has no code";
        return false;
    }
    code = invite->attrOr("code");
    return true;
}

bool parseGroupAcceptInvite(const BinaryNode &iq, std::string &groupJid, std::string &error) {
    groupJid.clear();
    const std::string err = iqErrorText(iq);
    if (!err.empty()) {
        error = "join group failed: " + err;
        return false;
    }
    const BinaryNode *group = iq.child("group");
    if (!group || group->attrOr("jid").empty()) {
        error = "join group reply has no group jid";
        return false;
    }
    groupJid = group->attrOr("jid");
    return true;
}

// ── Group sending (sender keys) ───────────────────────────────────────────

bool buildGroupMessageStanza(signal::Store &sessions, signal::SenderKeyStore &senderKeys,
                             const std::string &meJid, const std::string &groupJid,
                             const waproto::Message &message, const std::string &msgId,
                             const std::vector<DeviceBundle> &bundles,
                             const std::set<std::string> &alreadyDistributed,
                             GroupSendPlan &out, std::string &error) {
    out = GroupSendPlan();
    if (bundles.empty()) {
        error = "no device bundles for group members";
        return false;
    }
    if (groupJid.empty()) {
        error = "empty group jid";
        return false;
    }

    // Our chain for this group (created on first send) + the message body.
    signal::SenderKeyDistribution dist;
    if (!signal::createSenderKey(senderKeys, groupJid, meJid, dist, error)) return false;

    const Bytes padded = waproto::padMessage(waproto::encodeMessage(message));

    Bytes skmsg;
    if (!signal::encryptGroup(senderKeys, groupJid, meJid, padded, skmsg, error)) return false;

    out.distribution = signal::encodeDistribution(dist);

    BinaryNode stanza("message");
    stanza.setAttr("id", msgId);
    stanza.setAttr("type", message.mediaType().empty() ? "text" : "media");
    stanza.setAttr("to", groupJid);

    // The body itself, encrypted once with our sender key.
    BinaryNode enc("enc");
    enc.setAttr("v", "2");
    enc.setAttr("type", "skmsg");
    enc.setBytes(skmsg);
    stanza.addChild(enc);

    // The distribution message goes to every device that does not have it yet,
    // inside a normal 1:1 encrypted message.
    waproto::Message distMessage;
    distMessage.hasSenderKeyDistribution = true;
    distMessage.senderKeyDistribution.groupId = groupJid;
    distMessage.senderKeyDistribution.axolotl = out.distribution;

    BinaryNode participants("participants");
    for (const auto &bundle : bundles) {
        const bool isSelf = sameUser(bundle.jid, meJid);
        if (!isSelf && alreadyDistributed.count(bundle.jid)) continue;

        BinaryNode toNode;
        bool usedPreKey = false;
        if (!encryptForDevice(sessions, bundle, distMessage, false, groupJid, toNode,
                              usedPreKey, error)) {
            return false;
        }
        participants.addChild(toNode);
        out.distributedTo.push_back(bundle.jid);
        out.deviceCount++;
    }
    if (participants.hasContent()) stanza.addChild(participants);

    out.stanza = stanza;
    return true;
}

bool buildGroupTextStanza(signal::Store &sessions, signal::SenderKeyStore &senderKeys,
                          const std::string &meJid, const std::string &groupJid,
                          const std::string &text, const std::string &msgId,
                          const std::vector<DeviceBundle> &bundles,
                          const std::set<std::string> &alreadyDistributed,
                          GroupSendPlan &out, std::string &error) {
    waproto::Message message;
    if (!text.empty()) message.conversation = text;
    return buildGroupMessageStanza(sessions, senderKeys, meJid, groupJid, message, msgId,
                                   bundles, alreadyDistributed, out, error);
}

// ── Incoming messages ─────────────────────────────────────────────────────

bool decodeMessageNode(const BinaryNode &node, const std::string &meJid,
                       IncomingMessage &out, std::string &error) {
    out = IncomingMessage();
    out.id = node.attrOr("id");
    const std::string from = node.attrOr("from");
    const std::string participant = node.attrOr("participant");
    const std::string recipient = node.attrOr("recipient");
    out.pushName = node.attrOr("notify");
    out.participant = participant;
    out.recipient = recipient;

    if (from.empty()) {
        error = "message stanza without from";
        return false;
    }
    const std::string t = node.attrOr("t");
    if (!t.empty()) out.timestamp = std::strtoull(t.c_str(), nullptr, 10);

    if (isJidGroup(from) || isJidBroadcast(from)) {
        if (participant.empty()) {
            error = "group message without participant";
            return false;
        }
        out.isGroup = true;
        out.chatJid = from;
        out.senderJid = participant;
    } else if (isJidUser(from) || isJidNewsletter(from) ||
               from.find("@lid") != std::string::npos) {
        // Our own device echoes carry `recipient` = the real chat.
        const bool fromIsMe = sameUser(from, meJid);
        out.chatJid = (!recipient.empty() && fromIsMe) ? recipient : from;
        out.senderJid = from;
    } else {
        error = "unknown message sender: " + from;
        return false;
    }
    out.fromMe = sameUser(out.senderJid, meJid);
    return true;
}

bool decryptMessageNode(signal::Store &store, signal::SenderKeyStore *senderKeys,
                        const BinaryNode &node, const std::string &meJid,
                        IncomingMessage &out, std::string &error) {
    if (!decodeMessageNode(node, meJid, out, error)) return false;

    std::vector<const BinaryNode *> encNodes = node.childrenNamed("enc");
    if (const BinaryNode *single = node.child("enc")) {
        if (std::find(encNodes.begin(), encNodes.end(), single) == encNodes.end())
            encNodes.push_back(single);
    }
    const BinaryNode *plaintextNode = node.child("plaintext");
    if (encNodes.empty() && !plaintextNode) {
        error = "message has no decryptable payload";
        return false;
    }

    // The session address is the sender's user jid (device stripped).
    const std::string address = jidNormalizedUser(out.senderJid);

    for (const BinaryNode *enc : encNodes) {
        if (enc->contentType != BinaryNode::Content::Bytes || enc->bytes.empty())
            continue;
        const std::string type = enc->attrOr("type");
        Bytes plaintext;
        std::string decryptError;
        bool ok = false;

        if (type == "msg") {
            ok = signal::decrypt(store, address, 1, enc->bytes, plaintext, decryptError);
        } else if (type == "pkmsg") {
            ok = signal::decrypt(store, address, 3, enc->bytes, plaintext, decryptError);
        } else if (type == "skmsg") {
            if (!senderKeys) {
                decryptError = "group message without a sender-key store";
            } else {
                // Group messages are signed/keyed by (group, author).
                ok = signal::decryptGroup(*senderKeys, out.chatJid, out.senderJid, enc->bytes,
                                          plaintext, decryptError);
            }
        } else {
            decryptError = "unknown enc type: " + type;
        }

        if (!ok) {
            out.error = decryptError;
            continue;
        }

        Bytes unpadded;
        if (!waproto::unpadMessage(plaintext, unpadded)) {
            out.error = "message padding invalid";
            continue;
        }
        waproto::Message message;
        if (!waproto::decodeMessage(unpadded, message)) {
            out.error = "Message protobuf did not parse";
            continue;
        }
        // Own-device copies are wrapped; the inner payload is the real message.
        if (message.hasDeviceSent) {
            waproto::Message inner;
            if (waproto::decodeMessage(message.deviceSentPayload, inner)) {
                if (!message.deviceSentDestinationJid.empty() && !out.isGroup)
                    out.chatJid = message.deviceSentDestinationJid;
                message = inner;
            }
        }

        // A sender-key distribution message from a group member: remember the
        // chain so their subsequent skmsg bodies can be decrypted.
        if (message.hasSenderKeyDistribution && senderKeys) {
            const std::string group = message.senderKeyDistribution.groupId.empty()
                                          ? out.chatJid
                                          : message.senderKeyDistribution.groupId;
            signal::SenderKeyDistribution dist;
            std::string distError;
            if (signal::decodeDistribution(message.senderKeyDistribution.axolotl, dist,
                                           distError) &&
                signal::processDistribution(*senderKeys, group, out.senderJid, dist,
                                            distError)) {
                out.receivedSenderKey = true;
            } else {
                out.error = "sender key distribution: " + distError;
            }
        }

        out.message = message;
        out.text = message.text();
        out.decrypted = true;
        out.error.clear();
        if (message.hasProtocolMessage && message.hasHistorySync) {
            out.hasHistorySync = true;
            out.historySync = message.historySync;
        }
        return true;
    }

    if (plaintextNode && plaintextNode->contentType == BinaryNode::Content::Bytes) {
        waproto::Message message;
        if (waproto::decodeMessage(plaintextNode->bytes, message)) {
            out.message = message;
            out.text = message.text();
            out.decrypted = true;
            out.error.clear();
            return true;
        }
    }

    if (out.error.empty()) out.error = "no payload could be decrypted";
    error = out.error;
    return false;
}

std::string generateMessageId() {
    const Bytes rnd = crypto::randomBytes(8);
    std::string hex = crypto::toHex(rnd);
    std::string upper;
    for (char c : hex) upper += char(std::toupper(static_cast<unsigned char>(c)));
    return "3EB0" + upper;
}

} // namespace messaging
} // namespace wa
