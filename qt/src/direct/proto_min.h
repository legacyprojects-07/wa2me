// Minimal protobuf support for the WhatsApp handshake.
//
// Baileys generates its protobuf code from WAProto/WAProto.proto with
// protobufjs. Requiring protoc + protobuf-lite on Symbian is painful, so the
// handful of messages needed *before* a session exists (handshake, client
// payload, cert chain, device props) are hand-coded here with a tiny writer
// and reader. The post-login messages (Message, HistorySync, …) live in
// wa_proto.h on top of the same Writer/Reader; AppStateSync patches are still
// missing (QT_PORT.md §2.3).
//
// Encoding is verified byte-for-byte against Baileys' generated proto code
// in qt/tests/test_proto.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wa {
namespace proto {

using Bytes = std::vector<uint8_t>;

// ── Generic writer ─────────────────────────────────────────────────────────

class Writer {
public:
    void varint(uint64_t v);
    void tag(uint32_t field, uint32_t wireType);
    void fieldVarint(uint32_t field, uint64_t v);
    void fieldBool(uint32_t field, bool v);
    void fieldBytes(uint32_t field, const Bytes &v);
    void fieldString(uint32_t field, const std::string &v);
    // Writes a length-delimited sub-message (embedded message).
    void fieldMessage(uint32_t field, const Bytes &encoded);
    // Writes a packed repeated varint field.
    void fieldPackedVarints(uint32_t field, const std::vector<uint32_t> &values);

    const Bytes &data() const { return buf_; }

private:
    Bytes buf_;
};

// ── Generic reader ─────────────────────────────────────────────────────────

struct Field {
    uint32_t number = 0;
    uint32_t wireType = 0;
    uint64_t varint = 0;
    Bytes bytes;
    size_t start = 0;   // offset of the field's payload (for embedded messages)
    size_t end = 0;
};

class Reader {
public:
    explicit Reader(const Bytes &data) : data_(data) {}
    Reader(const uint8_t *data, size_t len) : data_(data, data + len) {}

    bool next(Field &out);
    bool atEnd() const { return pos_ >= data_.size(); }
    // Returns the raw slice of a length-delimited field (for nested parse).
    Bytes slice(const Field &f) const {
        return Bytes(data_.begin() + f.start, data_.begin() + f.end);
    }
    const Bytes &data() const { return data_; }

private:
    bool readVarint(uint64_t &v);
    Bytes data_;
    size_t pos_ = 0;
};

// ── HandshakeMessage (WAProto.proto:1459) ─────────────────────────────────

struct ServerHello {
    Bytes ephemeral;
    Bytes staticKey;
    Bytes payload;
};

struct ClientHello {
    Bytes ephemeral;
    Bytes staticKey;
    Bytes payload;
};

struct ClientFinish {
    Bytes staticKey;
    Bytes payload;
};

Bytes encodeClientHello(const ClientHello &hello);
bool decodeServerHello(const Bytes &bytes, ServerHello &out);
Bytes encodeClientFinish(const ClientFinish &finish);

// ── CertChain (WAProto.proto) ─────────────────────────────────────────────
struct CertChainDetails {
    bool ok = false;
    uint64_t serial = 0;
    std::string issuerSerialRaw;
};

bool decodeCertChainIssuerSerial(const Bytes &certChainBytesFromNoise,
                                 uint64_t &serialOut);

// ── ClientPayload (WAProto.proto:774) ─────────────────────────────────────

struct AppVersion {
    uint32_t primary = 0;
    uint32_t secondary = 0;
    uint32_t tertiary = 0;
};

struct UserAgent {
    uint32_t platform = 1;          // WEB
    AppVersion appVersion;
    std::string mcc = "000";
    std::string mnc = "000";
    std::string osVersion = "0.1";
    std::string manufacturer;
    std::string device = "Desktop";
    std::string osBuildNumber = "0.1";
    std::string phoneId;
    uint32_t releaseChannel = 0;    // RELEASE
    std::string localeLanguageIso6391 = "en";
    std::string localeCountryIso31661Alpha2 = "US";
};

struct DevicePairingData {
    Bytes eRegid;
    Bytes eKeytype;      // 0x05 (KEY_BUNDLE_TYPE)
    Bytes eIdent;
    Bytes eSkeyId;
    Bytes eSkeyVal;
    Bytes eSkeySig;
    Bytes buildHash;     // md5(version string)
    Bytes deviceProps;   // encoded DeviceProps
};

struct ClientPayload {
    uint64_t username = 0;       // set when logging in (existing session)
    // Baileys builds these payloads from an object literal, so fields that are
    // present are serialised even when zero. These flags mirror that.
    bool hasPassive = true;
    bool passive = false;
    bool hasPull = true;
    bool pull = false;
    bool hasDevice = false;
    uint32_t device = 0;
    uint32_t connectType = 1;    // WIFI_UNKNOWN
    uint32_t connectReason = 1;  // USER_ACTIVATED
    bool hasUserAgent = true;
    UserAgent userAgent;
    uint32_t webSubPlatform = 0; // WEB_BROWSER
    bool hasDevicePairingData = false;
    DevicePairingData devicePairingData;
};

Bytes encodeClientPayload(const ClientPayload &payload);

// DeviceProps (WAProto.proto:1271) — embedded in DevicePairingData.
struct HistorySyncConfig {
    uint32_t storageQuotaMb = 10240;
    bool inlineInitialPayloadInE2EeMsg = true;
    bool supportCallLogHistory = false;
    bool supportBotUserAgentChatHistory = true;
    bool supportCagReactionsAndPolls = true;
    bool supportBizHostedMsg = true;
    bool supportRecentSyncChunkMessageCountTuning = true;
    bool supportHostedGroupMsg = true;
    bool supportFbidBotChatHistory = true;
    bool supportMessageAssociation = true;
};

Bytes encodeDeviceProps(const std::string &os, uint32_t platformType,
                        bool requireFullSync, const HistorySyncConfig &historySync);

// Big-endian fixed-width integer encoding used in devicePairingData.
Bytes encodeBigEndian(uint64_t value, int bytes = 0);

// MD5, needed for the app-version build hash (Baileys: md5(version.join('.'))).
Bytes md5(const Bytes &data);

} // namespace proto
} // namespace wa
