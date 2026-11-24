#include "proto_min.h"

#include <cstring>

namespace wa {
namespace proto {

// ── Writer ─────────────────────────────────────────────────────────────────

void Writer::varint(uint64_t v) {
    while (v >= 0x80) {
        buf_.push_back(uint8_t((v & 0x7f) | 0x80));
        v >>= 7;
    }
    buf_.push_back(uint8_t(v));
}

void Writer::tag(uint32_t field, uint32_t wireType) {
    varint((uint64_t(field) << 3) | wireType);
}

void Writer::fieldVarint(uint32_t field, uint64_t v) {
    tag(field, 0);
    varint(v);
}

void Writer::fieldBool(uint32_t field, bool v) { fieldVarint(field, v ? 1 : 0); }

void Writer::fieldBytes(uint32_t field, const Bytes &v) {
    tag(field, 2);
    varint(v.size());
    buf_.insert(buf_.end(), v.begin(), v.end());
}

void Writer::fieldString(uint32_t field, const std::string &v) {
    fieldBytes(field, Bytes(v.begin(), v.end()));
}

void Writer::fieldMessage(uint32_t field, const Bytes &encoded) { fieldBytes(field, encoded); }

void Writer::fieldPackedVarints(uint32_t field, const std::vector<uint32_t> &values) {
    Bytes packed;
    for (uint32_t v : values) {
        while (v >= 0x80) {
            packed.push_back(uint8_t((v & 0x7f) | 0x80));
            v >>= 7;
        }
        packed.push_back(uint8_t(v));
    }
    fieldBytes(field, packed);
}

// ── Reader ─────────────────────────────────────────────────────────────────

bool Reader::readVarint(uint64_t &v) {
    v = 0;
    int shift = 0;
    while (pos_ < data_.size()) {
        const uint8_t b = data_[pos_++];
        v |= uint64_t(b & 0x7f) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

bool Reader::next(Field &out) {
    if (atEnd()) return false;
    uint64_t key;
    if (!readVarint(key)) return false;
    out.number = uint32_t(key >> 3);
    out.wireType = uint32_t(key & 0x7);
    out.varint = 0;
    out.bytes.clear();

    switch (out.wireType) {
    case 0:   // varint
        if (!readVarint(out.varint)) return false;
        return true;
    case 1:   // fixed64
        if (pos_ + 8 > data_.size()) return false;
        pos_ += 8;
        return true;
    case 2: { // length-delimited
        uint64_t len;
        if (!readVarint(len)) return false;
        if (pos_ + len > data_.size()) return false;
        out.start = pos_;
        out.end = pos_ + size_t(len);
        out.bytes.assign(data_.begin() + pos_, data_.begin() + pos_ + size_t(len));
        pos_ += size_t(len);
        return true;
    }
    case 5:   // fixed32
        if (pos_ + 4 > data_.size()) return false;
        pos_ += 4;
        return true;
    default:
        return false;
    }
}

// ── HandshakeMessage ───────────────────────────────────────────────────────

Bytes encodeClientHello(const ClientHello &hello) {
    Writer inner;
    if (!hello.ephemeral.empty()) inner.fieldBytes(1, hello.ephemeral);
    if (!hello.staticKey.empty()) inner.fieldBytes(2, hello.staticKey);
    if (!hello.payload.empty()) inner.fieldBytes(3, hello.payload);

    Writer outer;
    outer.fieldMessage(2, inner.data());   // clientHello = 2
    return outer.data();
}

bool decodeServerHello(const Bytes &bytes, ServerHello &out) {
    Reader outer(bytes);
    Field f;
    bool found = false;
    while (outer.next(f)) {
        if (f.number == 3 && f.wireType == 2) {   // serverHello = 3
            Reader inner(outer.slice(f));
            Field g;
            while (inner.next(g)) {
                if (g.wireType != 2) continue;
                if (g.number == 1) out.ephemeral = g.bytes;
                else if (g.number == 2) out.staticKey = g.bytes;
                else if (g.number == 3) out.payload = g.bytes;
            }
            found = true;
        }
    }
    return found && !out.ephemeral.empty();
}

Bytes encodeClientFinish(const ClientFinish &finish) {
    Writer inner;
    if (!finish.staticKey.empty()) inner.fieldBytes(1, finish.staticKey);
    if (!finish.payload.empty()) inner.fieldBytes(2, finish.payload);

    Writer outer;
    outer.fieldMessage(4, inner.data());   // clientFinish = 4
    return outer.data();
}

// ── CertChain ──────────────────────────────────────────────────────────────

bool decodeCertChainIssuerSerial(const Bytes &certChainBytes, uint64_t &serialOut) {
    // CertChain: { NoiseCertificate leaf = 1; NoiseCertificate intermediate = 2; }
    // NoiseCertificate: { bytes details = 1; bytes signature = 2; }
    // Details: { string serial = 1; string issuerSerial = 2; ... }
    Reader chain(certChainBytes);
    Field f;
    bool ok = false;
    while (chain.next(f)) {
        if (f.number != 2 || f.wireType != 2) continue;   // intermediate
        Reader cert(chain.slice(f));
        Field g;
        while (cert.next(g)) {
            if (g.number != 1 || g.wireType != 2) continue;   // details
            Reader details(cert.slice(g));
            Field h;
            while (details.next(h)) {
                if (h.number == 1 && h.wireType == 2) {
                    // serial is a string holding a number
                    std::string s(h.bytes.begin(), h.bytes.end());
                    try {
                        serialOut = std::stoull(s);
                        ok = true;
                    } catch (...) {
                        ok = false;
                    }
                }
            }
        }
    }
    return ok;
}

// ── ClientPayload ──────────────────────────────────────────────────────────

static Bytes encodeUserAgent(const UserAgent &ua) {
    Writer w;
    w.fieldVarint(1, ua.platform);
    {
        Writer ver;
        ver.fieldVarint(1, ua.appVersion.primary);
        ver.fieldVarint(2, ua.appVersion.secondary);
        ver.fieldVarint(3, ua.appVersion.tertiary);
        w.fieldMessage(2, ver.data());
    }
    w.fieldString(3, ua.mcc);
    w.fieldString(4, ua.mnc);
    w.fieldString(5, ua.osVersion);
    if (!ua.manufacturer.empty()) w.fieldString(6, ua.manufacturer);
    w.fieldString(7, ua.device);
    w.fieldString(8, ua.osBuildNumber);
    if (!ua.phoneId.empty()) w.fieldString(9, ua.phoneId);
    w.fieldVarint(10, ua.releaseChannel);
    w.fieldString(11, ua.localeLanguageIso6391);
    w.fieldString(12, ua.localeCountryIso31661Alpha2);
    return w.data();
}

static Bytes encodeDevicePairing(const DevicePairingData &d) {
    Writer w;
    if (!d.eRegid.empty()) w.fieldBytes(1, d.eRegid);
    if (!d.eKeytype.empty()) w.fieldBytes(2, d.eKeytype);
    if (!d.eIdent.empty()) w.fieldBytes(3, d.eIdent);
    if (!d.eSkeyId.empty()) w.fieldBytes(4, d.eSkeyId);
    if (!d.eSkeyVal.empty()) w.fieldBytes(5, d.eSkeyVal);
    if (!d.eSkeySig.empty()) w.fieldBytes(6, d.eSkeySig);
    if (!d.buildHash.empty()) w.fieldBytes(7, d.buildHash);
    if (!d.deviceProps.empty()) w.fieldBytes(8, d.deviceProps);
    return w.data();
}

Bytes encodeClientPayload(const ClientPayload &p) {
    Writer w;
    if (p.username != 0) w.fieldVarint(1, p.username);
    // Field order follows Baileys/protobufjs output (ascending field number).
    if (p.hasPassive) w.fieldBool(3, p.passive);

    if (p.hasUserAgent) {
        w.fieldMessage(5, encodeUserAgent(p.userAgent));
        Writer webInfo;
        webInfo.fieldVarint(4, p.webSubPlatform);
        w.fieldMessage(6, webInfo.data());
    }

    w.fieldVarint(12, p.connectType);
    w.fieldVarint(13, p.connectReason);
    if (p.hasDevice) w.fieldVarint(18, p.device);
    if (p.hasDevicePairingData) {
        w.fieldMessage(19, encodeDevicePairing(p.devicePairingData));
    }
    if (p.hasPull) w.fieldBool(33, p.pull);
    return w.data();
}

Bytes encodeDeviceProps(const std::string &os, uint32_t platformType,
                        bool requireFullSync, const HistorySyncConfig &h) {
    Writer w;
    w.fieldString(1, os);
    w.fieldVarint(3, platformType);
    w.fieldBool(4, requireFullSync);

    // Field numbers per WAProto.proto HistorySyncConfig (lines 1285-1300).
    Writer hs;
    hs.fieldVarint(3, h.storageQuotaMb);
    hs.fieldBool(4, h.inlineInitialPayloadInE2EeMsg);
    hs.fieldBool(6, h.supportCallLogHistory);
    hs.fieldBool(7, h.supportBotUserAgentChatHistory);
    hs.fieldBool(8, h.supportCagReactionsAndPolls);
    hs.fieldBool(9, h.supportBizHostedMsg);
    hs.fieldBool(10, h.supportRecentSyncChunkMessageCountTuning);
    hs.fieldBool(11, h.supportHostedGroupMsg);
    hs.fieldBool(12, h.supportFbidBotChatHistory);
    hs.fieldBool(14, h.supportMessageAssociation);
    w.fieldMessage(5, hs.data());
    return w.data();
}

Bytes encodeBigEndian(uint64_t value, int bytes) {
    Bytes out;
    if (bytes == 0) {
        // Minimal big-endian representation (Baileys' encodeBigEndian default).
        if (value == 0) return Bytes{0};
        while (value > 0) {
            out.insert(out.begin(), uint8_t(value & 0xff));
            value >>= 8;
        }
        return out;
    }
    out.resize(size_t(bytes), 0);
    for (int i = 0; i < bytes; i++) {
        out[size_t(bytes - 1 - i)] = uint8_t((value >> (8 * i)) & 0xff);
    }
    return out;
}

// ── MD5 (only used for the app-version build hash) ────────────────────────

namespace {
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    static const uint32_t K[64];
    static const int S[64];

    void process(const uint8_t block[64]) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = uint32_t(block[4 * i]) | (uint32_t(block[4 * i + 1]) << 8) |
                   (uint32_t(block[4 * i + 2]) << 16) | (uint32_t(block[4 * i + 3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + m[g];
            A = D;
            D = C;
            C = B;
            B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
};

const uint32_t Md5::K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

const int Md5::S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
} // namespace

Bytes md5(const Bytes &data) {
    Md5 md;
    Bytes msg = data;
    const uint64_t bitLen = uint64_t(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 0; i < 8; i++) msg.push_back(uint8_t((bitLen >> (8 * i)) & 0xff));

    for (size_t off = 0; off < msg.size(); off += 64) md.process(msg.data() + off);

    Bytes out(16);
    const uint32_t words[4] = {md.a0, md.b0, md.c0, md.d0};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t((words[i] >> (8 * j)) & 0xff);
    }
    return out;
}

} // namespace proto
} // namespace wa
