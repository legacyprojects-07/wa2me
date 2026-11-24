// WhatsApp BinaryNode codec — faithful C++ port of Baileys 6.7.22
// (lib/WABinary/{encode,decode,constants,jid-utils}.js).
//
// Pure C++17, no Qt, no external deps except zlib for compressed frames
// (WhatsApp compresses frames with raw DEFLATE after the Noise handshake).
// Being Qt-free means it is unit-testable on a desktop toolchain and
// trivially portable to Symbian's Qt build.
//
// Wire format summary
// -------------------
//   frame    := flag_byte payload
//               flag bit 0x02 set -> payload is raw-DEFLATE compressed
//   node     := listSize tagString (attrKey attrValue)* [content]
//   listSize := 2*attrCount + 1 + (hasContent ? 1 : 0)
//   string   := token | dictionary-token | packed-nibble | packed-hex |
//               jid-pair | ad-jid | byte-length-prefixed utf8
//   list     := LIST_EMPTY | LIST_8 n | LIST_16 n  followed by n nodes

#pragma once

#include <stdint.h>
#include "compat.h"
#include <string>
#include <vector>
#include <utility>

namespace wa {

// ── JID helpers (jid-utils.js) ──────────────────────────────────────────────

struct Jid {
    std::string user;
    std::string server;
    int device;  // -1 = absent  (":" in the user part)
    int agent;  // -1 = absent  ("_" in the user part)
    bool valid;
    Jid() : device(-1), agent(-1), valid(false) {}
};

Jid jidDecode(const std::string &jid);
std::string jidEncode(const std::string &user, const std::string &server,
                      int device = -1, int agent = -1);
// Strips device/agent and maps @c.us -> @s.whatsapp.net (jidNormalizedUser).
std::string jidNormalizedUser(const std::string &jid);
bool isJidGroup(const std::string &jid);
bool isJidUser(const std::string &jid);
bool isJidBroadcast(const std::string &jid);
bool isJidNewsletter(const std::string &jid);
bool isJidStatusBroadcast(const std::string &jid);
bool areJidsSameUser(const std::string &a, const std::string &b);

// ── BinaryNode ─────────────────────────────────────────────────────────────

struct BinaryNode {
    enum class Content { None, Text, Bytes, List };

    std::string tag;
    // Insertion-ordered attributes: preserves round-trip byte equality with
    // Baileys, which serialises attributes in object-key order.
    std::vector<std::pair<std::string, std::string>> attrs;

    Content contentType;
    std::string text;                  // Content::Text
    std::vector<uint8_t> bytes;        // Content::Bytes
    std::vector<BinaryNode> children;  // Content::List

    BinaryNode() : contentType(Content::None) {}
    explicit BinaryNode(std::string t) : tag(compat::move(t)), contentType(Content::None) {}
    BinaryNode(std::string t, std::vector<std::pair<std::string, std::string>> a)
        : tag(compat::move(t)), attrs(compat::move(a)), contentType(Content::None) {}

    // Attribute helpers (set/get/replace, order-preserving).
    const std::string *attr(const std::string &key) const;
    std::string attrOr(const std::string &key, const std::string &def = "") const;
    void setAttr(const std::string &key, const std::string &value);
    void removeAttr(const std::string &key);
    bool hasAttr(const std::string &key) const;

    // Content helpers.
    BinaryNode &setText(std::string t);
    BinaryNode &setBytes(std::vector<uint8_t> b);
    BinaryNode &setList(std::vector<BinaryNode> kids);
    BinaryNode &addChild(BinaryNode child);
    bool hasContent() const { return contentType != Content::None; }

    // Child lookup across both List content and a single child.
    const BinaryNode *child(const std::string &tagName) const;
    std::vector<const BinaryNode *> childrenNamed(const std::string &tagName) const;
};

// ── Codec ──────────────────────────────────────────────────────────────────

namespace binary {

// Encodes a node, including the leading "not compressed" flag byte (0x00).
std::vector<uint8_t> encode(const BinaryNode &node);

// Encodes without the leading flag byte (what the Noise layer hands over).
std::vector<uint8_t> encodePayload(const BinaryNode &node);

struct DecodeResult {
    bool ok;
    BinaryNode node;
    std::string error;
    DecodeResult() : ok(false) {}
};

// Decodes a complete frame (flag byte + payload). `consumed` receives the
// number of bytes used, so multi-frame buffers can be split.
DecodeResult decode(const uint8_t *data, size_t len, size_t *consumed = nullptr);
DecodeResult decode(const std::vector<uint8_t> &frame, size_t *consumed = nullptr);

// Nibble/hex packing helpers (exposed for tests).
std::vector<uint8_t> packBytes(const std::string &str, bool nibble);
std::string unpackBytes(const std::vector<uint8_t> &packed, bool nibble);

} // namespace binary
} // namespace wa
