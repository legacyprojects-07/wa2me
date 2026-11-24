#include "binary_node.h"

#include "compat.h"
#include "inflate.h"
#include "wa_tokens.h"

#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <algorithm>

namespace wa {

// ─── JID helpers ─────────────────────────────────────────────────────────────

std::string numToString(long long v) { return compat::toString(v); }

Jid jidDecode(const std::string &jid) {
    Jid out;
    const size_t sep = jid.find('@');
    if (sep == std::string::npos) return out;         // invalid (no server)
    out.server = jid.substr(sep + 1);
    std::string userCombined = jid.substr(0, sep);
    std::string userAgent = userCombined, deviceStr;
    const size_t colon = userCombined.find(':');
    if (colon != std::string::npos) {
        userAgent = userCombined.substr(0, colon);
        deviceStr = userCombined.substr(colon + 1);
    }
    const size_t underscore = userAgent.find('_');
    if (underscore != std::string::npos) {
        out.agent = std::atoi(userAgent.c_str() + underscore + 1);
        out.user = userAgent.substr(0, underscore);
    } else {
        out.user = userAgent;
    }
    if (!deviceStr.empty()) out.device = std::atoi(deviceStr.c_str());
    out.valid = true;
    return out;
}

std::string jidEncode(const std::string &user, const std::string &server,
                      int device, int agent) {
    std::string out = user;
    if (agent > 0) out += "_" + numToString(agent);
    if (device > 0) out += ":" + numToString(device);
    out += "@" + server;
    return out;
}

std::string jidNormalizedUser(const std::string &jid) {
    Jid j = jidDecode(jid);
    if (!j.valid) return std::string();
    const std::string server = (j.server == "c.us") ? "s.whatsapp.net" : j.server;
    return jidEncode(j.user, server);
}

static bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isJidGroup(const std::string &jid) { return endsWith(jid, "@g.us"); }
bool isJidUser(const std::string &jid) { return endsWith(jid, "@s.whatsapp.net"); }
bool isJidBroadcast(const std::string &jid) { return endsWith(jid, "@broadcast"); }
bool isJidNewsletter(const std::string &jid) { return endsWith(jid, "@newsletter"); }
bool isJidStatusBroadcast(const std::string &jid) { return jid == "status@broadcast"; }

bool areJidsSameUser(const std::string &a, const std::string &b) {
    Jid ja = jidDecode(a), jb = jidDecode(b);
    return ja.valid && jb.valid && ja.user == jb.user;
}

// ─── BinaryNode helpers ─────────────────────────────────────────────────────

const std::string *BinaryNode::attr(const std::string &key) const {
    for (const auto &kv : attrs) if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string BinaryNode::attrOr(const std::string &key, const std::string &def) const {
    const std::string *v = attr(key);
    return v ? *v : def;
}

bool BinaryNode::hasAttr(const std::string &key) const { return attr(key) != nullptr; }

void BinaryNode::setAttr(const std::string &key, const std::string &value) {
    for (auto &kv : attrs) {
        if (kv.first == key) { kv.second = value; return; }
    }
    attrs.push_back(std::make_pair(key, value));
}

void BinaryNode::removeAttr(const std::string &key) {
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if (it->first == key) { attrs.erase(it); return; }
    }
}

BinaryNode &BinaryNode::setText(std::string t) {
    contentType = Content::Text; text = compat::move(t); bytes.clear(); children.clear();
    return *this;
}

BinaryNode &BinaryNode::setBytes(std::vector<uint8_t> b) {
    contentType = Content::Bytes; bytes = compat::move(b); text.clear(); children.clear();
    return *this;
}

BinaryNode &BinaryNode::setList(std::vector<BinaryNode> kids) {
    contentType = Content::List; children = compat::move(kids); text.clear(); bytes.clear();
    return *this;
}

BinaryNode &BinaryNode::addChild(BinaryNode c) {
    if (contentType != Content::List) {
        contentType = Content::List;
        text.clear();
        bytes.clear();
    }
    children.push_back(compat::move(c));
    return *this;
}

const BinaryNode *BinaryNode::child(const std::string &tagName) const {
    if (contentType != Content::List) return nullptr;
    for (const auto &c : children) if (c.tag == tagName) return &c;
    return nullptr;
}

std::vector<const BinaryNode *> BinaryNode::childrenNamed(const std::string &tagName) const {
    std::vector<const BinaryNode *> out;
    if (contentType != Content::List) return out;
    for (const auto &c : children) if (c.tag == tagName) out.push_back(&c);
    return out;
}

// ─── Token lookup (TOKEN_MAP in Baileys) ────────────────────────────────────

namespace {

struct TokenRef { int dict; int index; };

const std::map<std::string, TokenRef> &tokenMap() {
    static std::map<std::string, TokenRef> *map = [] {
        auto *m = new std::map<std::string, TokenRef>();
        for (int i = 1; i < tokens::SINGLE_COUNT; ++i) {
            const char *s = tokens::SINGLE[i];
            if (s && *s && m->find(s) == m->end()) (*m)[s] = TokenRef{-1, i};
        }
        for (int d = 0; d < tokens::DICT_COUNT; ++d) {
            for (int i = 0; i < tokens::DICT_SIZE; ++i) {
                const char *s = tokens::DICT[d][i];
                if (s && *s && m->find(s) == m->end()) (*m)[s] = TokenRef{d, i};
            }
        }
        return m;
    }();
    return *map;
}

// Tag values (constants.js TAGS).
enum : int {
    LIST_EMPTY = 0,
    DICTIONARY_0 = 236,
    DICTIONARY_1 = 237,
    DICTIONARY_2 = 238,
    DICTIONARY_3 = 239,
    INTEROP_JID = 245,
    FB_JID = 246,
    AD_JID = 247,
    LIST_8 = 248,
    LIST_16 = 249,
    JID_PAIR = 250,
    HEX_8 = 251,
    BINARY_8 = 252,
    BINARY_20 = 253,
    BINARY_32 = 254,
    NIBBLE_8 = 255,
    PACKED_MAX = 127,
};

bool isNibble(const std::string &str) {
    if (str.empty() || (int)str.size() > PACKED_MAX) return false;
    for (char c : str) {
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return true;
}

bool isHex(const std::string &str) {
    if (str.empty() || (int)str.size() > PACKED_MAX) return false;
    for (char c : str) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

int packNibble(char c) {
    if (c == '-') return 10;
    if (c == '.') return 11;
    if (c == '\0') return 15;
    if (c >= '0' && c <= '9') return c - '0';
    throw std::runtime_error(std::string("invalid byte for nibble '") + c + "'");
}

int packHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c == '\0') return 15;
    throw std::runtime_error(std::string("invalid hex char '") + c + "'");
}

char unpackNibble(int v) {
    if (v >= 0 && v <= 9) return char('0' + v);
    if (v == 10) return '-';
    if (v == 11) return '.';
    if (v == 15) return '\0';
    throw std::runtime_error("invalid nibble: " + compat::toString(v));
}

char unpackHex(int v) {
    if (v >= 0 && v < 16) return v < 10 ? char('0' + v) : char('A' + v - 10);
    throw std::runtime_error("invalid hex: " + compat::toString(v));
}

// ── Encoder ────────────────────────────────────────────────────────────────

class Writer {
public:
    explicit Writer(std::vector<uint8_t> &buf) : b(buf) {}

    void byte(int v) { b.push_back(uint8_t(v & 0xff)); }
    void intN(uint64_t value, int n) {
        for (int i = 0; i < n; i++) {
            int shift = (n - 1 - i) * 8;
            b.push_back(uint8_t((value >> shift) & 0xff));
        }
    }
    void int20(uint32_t value) {
        b.push_back(uint8_t((value >> 16) & 0x0f));
        b.push_back(uint8_t((value >> 8) & 0xff));
        b.push_back(uint8_t(value & 0xff));
    }
    void raw(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }

    void byteLength(size_t length) {
        if (length >= (1u << 20)) {
            byte(BINARY_32);
            intN(length, 4);
        } else if (length >= 256) {
            byte(BINARY_20);
            int20(uint32_t(length));
        } else {
            byte(BINARY_8);
            byte(int(length));
        }
    }

    void stringRaw(const std::string &s) {
        byteLength(s.size());
        raw(reinterpret_cast<const uint8_t *>(wa::compat::dataOf(s)), s.size());
    }

    void jidValue(const Jid &j) {
        if (j.device >= 0) {
            byte(AD_JID);
            byte(j.server == "lid" ? 1 : 0);
            byte(j.device);
            string(j.user);
        } else {
            byte(JID_PAIR);
            if (!j.user.empty()) string(j.user);
            else byte(LIST_EMPTY);
            string(j.server);
        }
    }

    void packed(const std::string &str, bool nibble) {
        if ((int)str.size() > PACKED_MAX) throw std::runtime_error("Too many bytes to pack");
        byte(nibble ? NIBBLE_8 : HEX_8);
        int roundedLength = int((str.size() + 1) / 2);
        if (str.size() % 2 != 0) roundedLength |= 128;
        byte(roundedNumber(roundedLength));
        auto pack = nibble ? packNibble : packHex;
        size_t half = str.size() / 2;
        for (size_t i = 0; i < half; i++) {
            byte((pack(str[2 * i]) << 4) | pack(str[2 * i + 1]));
        }
        if (str.size() % 2 != 0) {
            byte((pack(str[str.size() - 1]) << 4) | pack('\0'));
        }
    }

    void string(const std::string &str) {
        const auto &map = tokenMap();
        auto it = map.find(str);
        if (it != map.end()) {
            if (it->second.dict >= 0) byte(DICTIONARY_0 + it->second.dict);
            byte(it->second.index);
            return;
        }
        if (isNibble(str)) { packed(str, true); return; }
        if (isHex(str)) { packed(str, false); return; }
        if (!str.empty()) {
            Jid j = jidDecode(str);
            if (j.valid) { jidValue(j); return; }
            stringRaw(str);
        }
        // Empty string: Baileys writes nothing (writeString falls through).
    }

    void listStart(int size) {
        if (size == 0) byte(LIST_EMPTY);
        else if (size < 256) { byte(LIST_8); byte(size); }
        else { byte(LIST_16); intN(uint16_t(size), 2); }
    }

private:
    static int roundedNumber(int v) { return v; }
    std::vector<uint8_t> &b;
};

void writeNode(const BinaryNode &node, Writer &w) {
    if (node.tag.empty()) throw std::runtime_error("Invalid node: tag cannot be undefined");

    // Baileys counts only attrs whose value is a string; our model stores
    // attributes as strings, so this is simply the attribute count.
    const int attrCount = int(node.attrs.size());
    const bool hasContent = node.hasContent();

    w.listStart(2 * attrCount + 1 + (hasContent ? 1 : 0));
    w.string(node.tag);
    for (const auto &kv : node.attrs) {
        w.string(kv.first);
        w.string(kv.second);
    }

    switch (node.contentType) {
    case BinaryNode::Content::None:
        break;
    case BinaryNode::Content::Text:
        w.string(node.text);
        break;
    case BinaryNode::Content::Bytes:
        w.byteLength(node.bytes.size());
        w.raw(wa::compat::dataOf(node.bytes), node.bytes.size());
        break;
    case BinaryNode::Content::List: {
        w.listStart(int(node.children.size()));
        for (const auto &c : node.children) writeNode(c, w);
        break;
    }
    }
}

// ── Decoder ────────────────────────────────────────────────────────────────

class Reader {
public:
    Reader(const uint8_t *d, size_t n) : data(d), len(n) {}

    int byte() {
        if (index + 1 > len) throw std::runtime_error("end of stream");
        return data[index++];
    }
    void ensure(size_t n) const {
        if (index + n > len) throw std::runtime_error("end of stream");
    }
    std::vector<uint8_t> bytes(size_t n) {
        ensure(n);
        std::vector<uint8_t> out(data + index, data + index + n);
        index += n;
        return out;
    }
    std::string chars(size_t n) {
        ensure(n);
        std::string s(reinterpret_cast<const char *>(data + index), n);
        index += n;
        return s;
    }
    uint32_t intN(int n) {
        ensure(size_t(n));
        uint32_t val = 0;
        for (int i = 0; i < n; i++) val |= uint32_t(byte()) << ((n - 1 - i) * 8);
        return val;
    }
    uint32_t int20() {
        ensure(3);
        return ((uint32_t(byte()) & 15) << 16) + (uint32_t(byte()) << 8) + uint32_t(byte());
    }

    size_t pos() const { return index; }

private:
    const uint8_t *data;
    size_t len;
    size_t index = 0;
};

std::string readPacked8(Reader &r, int tag) {
    const int startByte = r.byte();
    std::string value;
    auto unpack = [tag](int v) { return tag == NIBBLE_8 ? unpackNibble(v) : unpackHex(v); };
    for (int i = 0; i < (startByte & 127); i++) {
        const int cur = r.byte();
        value += unpack((cur & 0xf0) >> 4);
        value += unpack(cur & 0x0f);
    }
    if ((startByte >> 7) != 0 && !value.empty()) value.pop_back();
    return value;
}

std::string getTokenDouble(int dictIndex, int tokenIndex) {
    if (dictIndex < 0 || dictIndex >= tokens::DICT_COUNT)
        throw std::runtime_error("Invalid double token dict");
    if (tokenIndex < 0 || tokenIndex >= tokens::DICT_SIZE)
        throw std::runtime_error("Invalid double token");
    return tokens::DICT[dictIndex][tokenIndex];
}

std::string readString(Reader &r, int tag) {
    if (tag >= 1 && tag < tokens::SINGLE_COUNT) return tokens::SINGLE[tag];
    switch (tag) {
    case DICTIONARY_0:
    case DICTIONARY_1:
    case DICTIONARY_2:
    case DICTIONARY_3:
        return getTokenDouble(tag - DICTIONARY_0, r.byte());
    case LIST_EMPTY:
        return std::string();
    case BINARY_8:
        return r.chars(r.byte());
    case BINARY_20:
        return r.chars(r.int20());
    case BINARY_32:
        return r.chars(r.intN(4));
    case JID_PAIR: {
        const std::string i = readString(r, r.byte());
        const std::string j = readString(r, r.byte());
        if (!j.empty()) return (i.empty() ? std::string() : i) + "@" + j;
        throw std::runtime_error("invalid jid pair");
    }
    case AD_JID: {
        const int rawDomainType = r.byte();
        const int domainType = rawDomainType;
        const int device = r.byte();
        const std::string user = readString(r, r.byte());
        const std::string server = (domainType == 0 || domainType == 128)
                                       ? "s.whatsapp.net" : "lid";
        return jidEncode(user, server, device);
    }
    case HEX_8:
    case NIBBLE_8:
        return readPacked8(r, tag);
    default:
        throw std::runtime_error("invalid string with tag: " + compat::toString(tag));
    }
}

bool isListTag(int tag) { return tag == LIST_EMPTY || tag == LIST_8 || tag == LIST_16; }

int readListSize(Reader &r, int tag) {
    switch (tag) {
    case LIST_EMPTY: return 0;
    case LIST_8: return r.byte();
    case LIST_16: return int(r.intN(2));
    default: throw std::runtime_error("invalid tag for list size: " + compat::toString(tag));
    }
}

BinaryNode readNode(Reader &r) {
    const int listSize = readListSize(r, r.byte());
    const std::string header = readString(r, r.byte());
    if (listSize == 0 || header.empty()) throw std::runtime_error("invalid node");

    BinaryNode node;
    node.tag = header;

    const int attributesLength = (listSize - 1) >> 1;
    for (int i = 0; i < attributesLength; i++) {
        const std::string key = readString(r, r.byte());
        const std::string value = readString(r, r.byte());
        node.setAttr(key, value);
    }

    if (listSize % 2 == 0) {
        const int tag = r.byte();
        if (isListTag(tag)) {
            const int size = readListSize(r, tag);
            std::vector<BinaryNode> kids;
            kids.reserve(size_t(size));
            for (int i = 0; i < size; i++) kids.push_back(readNode(r));
            node.setList(compat::move(kids));
        } else {
            switch (tag) {
            case BINARY_8:
                node.setBytes(r.bytes(r.byte()));
                break;
            case BINARY_20:
                node.setBytes(r.bytes(r.int20()));
                break;
            case BINARY_32:
                node.setBytes(r.bytes(r.intN(4)));
                break;
            default:
                node.setText(readString(r, tag));
                break;
            }
        }
    }
    return node;
}

} // anonymous namespace

// ─── Public codec API ──────────────────────────────────────────────────────

namespace binary {

std::vector<uint8_t> encodePayload(const BinaryNode &node) {
    std::vector<uint8_t> out;
    Writer w(out);
    writeNode(node, w);
    return out;
}

std::vector<uint8_t> encode(const BinaryNode &node) {
    std::vector<uint8_t> out;
    out.push_back(0x00);   // not compressed
    Writer w(out);
    writeNode(node, w);
    return out;
}

DecodeResult decode(const uint8_t *data, size_t len, size_t *consumed) {
    DecodeResult res;
    try {
        if (len < 1) throw std::runtime_error("empty frame");
        std::vector<uint8_t> payload;
        size_t used = 0;
        if ((data[0] & 0x02) != 0) {
            std::vector<uint8_t> inflated;
            if (!inflateRaw(data + 1, len - 1, inflated))
                throw std::runtime_error("failed to inflate frame");
            payload.swap(inflated);
            used = len;
        } else {
            payload.assign(data + 1, data + len);
            used = len;
        }
        Reader r(wa::compat::dataOf(payload), payload.size());
        res.node = readNode(r);
        res.ok = true;
        if (consumed) *consumed = used;
    } catch (const std::exception &e) {
        res.ok = false;
        res.error = e.what();
    }
    return res;
}

DecodeResult decode(const std::vector<uint8_t> &frame, size_t *consumed) {
    return decode(wa::compat::dataOf(frame), frame.size(), consumed);
}

std::vector<uint8_t> packBytes(const std::string &str, bool nibble) {
    std::vector<uint8_t> out;
    Writer w(out);
    w.packed(str, nibble);
    return out;
}

std::string unpackBytes(const std::vector<uint8_t> &packedBytes, bool nibble) {
    Reader r(wa::compat::dataOf(packedBytes), packedBytes.size());
    return readPacked8(r, nibble ? NIBBLE_8 : HEX_8);
}

} // namespace binary
} // namespace wa
