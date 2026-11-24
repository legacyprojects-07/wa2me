#include "whatsapp_direct.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>

#include <set>

#include "aes_gcm.h"
#include "ed25519.h"
#include "media_crypto.h"
#include "proto_min.h"
#include "sha256.h"
#include "x25519.h"
#include "wa_proto.h"

using wa::BinaryNode;
using wa::proto::Bytes;

namespace {

// wss://web.whatsapp.com/ws/chat — DEFAULT_CONNECTION_CONFIG.waWebSocketUrl
const char *const kWebSocketUrl = "wss://web.whatsapp.com/ws/chat";
const char *const kOrigin = "https://web.whatsapp.com";

QByteArray toQ(const Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), int(b.size()));
}
Bytes toStd(const QByteArray &b) {
    return Bytes(reinterpret_cast<const uint8_t *>(b.constData()),
                 reinterpret_cast<const uint8_t *>(b.constData()) + size_t(b.size()));
}
QString b64(const QByteArray &b) { return QString::fromLatin1(b.toBase64()); }
QByteArray fromB64(const QString &s) { return QByteArray::fromBase64(s.toLatin1()); }

// Class-level logging so every line is visible in the direct-mode debug view.
#define DLOG(w, msg) (w)->logLine(QStringLiteral("[direct] ") + (msg))

} // namespace

// ── Signal store backed by the creds.json document ─────────────────────────
//
// wa::signal::Store needs identity/pre-keys/sessions; the Qt client keeps them
// in the same JSON file as the Noise key, which is what makes the account
// resumable after a restart (the wireless equivalent of Baileys' authState).
class WhatsAppDirect::CredsStore : public wa::signal::Store {
public:
    explicit CredsStore(WhatsAppDirect *owner) : q(owner) {}

    wa::signal::KeyPair ourIdentity() override {
        wa::signal::KeyPair kp;
        kp.priv = toStd(q->m_identityPriv);
        kp.pub = withPrefix(toStd(q->m_identityPub));
        return kp;
    }

    uint32_t ourRegistrationId() override { return q->m_registrationId; }

    bool loadPreKey(uint32_t id, wa::signal::KeyPair &out) override {
        for (const QJsonValue &v : q->m_preKeys) {
            const QJsonObject o = v.toObject();
            if (quint32(o.value("id").toInt()) != id) continue;
            out.priv = toStd(fromB64(o.value("priv").toString()));
            out.pub = withPrefix(toStd(fromB64(o.value("pub").toString())));
            return out.priv.size() == 32;
        }
        return false;
    }

    void removePreKey(uint32_t id) override {
        QJsonArray kept;
        for (const QJsonValue &v : q->m_preKeys) {
            if (quint32(v.toObject().value("id").toInt()) != id) kept.append(v);
        }
        q->m_preKeys = kept;
        q->saveCredentials();
    }

    bool loadSignedPreKey(uint32_t id, wa::signal::KeyPair &out) override {
        Q_UNUSED(id)
        out.priv = toStd(q->m_signedPreKeyPriv);
        out.pub = withPrefix(toStd(q->m_signedPreKeyPub));
        return out.priv.size() == 32;
    }

    wa::signal::Record loadSession(const std::string &address) override {
        wa::signal::Record record;
        const QJsonValue v = q->m_sessions.value(QString::fromStdString(address));
        if (v.isString()) {
            wa::signal::Record parsed;
            if (wa::signal::deserializeRecord(
                    wa::crypto::fromHex(v.toString().toStdString()), parsed)) {
                record = parsed;
            }
        }
        return record;
    }

    void storeSession(const std::string &address, const wa::signal::Record &record) override {
        q->m_sessions[QString::fromStdString(address)] = QString::fromStdString(
            wa::crypto::toHex(wa::signal::serializeRecord(record)));
        q->saveCredentials();
    }

private:
    // Public keys on the Signal wire carry WhatsApp's 0x05 bundle prefix.
    static Bytes withPrefix(const Bytes &raw) {
        if (raw.size() == 33) return raw;
        Bytes out;
        out.push_back(0x05);
        out.insert(out.end(), raw.begin(), raw.end());
        return out;
    }

    WhatsAppDirect *q;
};



// ── Group sender keys (one chain per group + member) ───────────────────────
//
// Group bodies are encrypted once with a per-sender chain key; only the
// distribution message is Signal-encrypted per device. The chains live in
// creds.json next to the 1:1 session records so they survive a restart.
class WhatsAppDirect::SenderKeysStore : public wa::signal::SenderKeyStore {
public:
    explicit SenderKeysStore(WhatsAppDirect *owner) : q(owner) {}

    bool loadSenderKey(const std::string &name, wa::signal::SenderKeyState &out) override {
        const QJsonValue v = q->m_senderKeyJson.value(QString::fromStdString(name));
        if (!v.isString()) return false;
        return wa::signal::deserializeSenderKeyState(v.toString().toStdString(), out);
    }

    void storeSenderKey(const std::string &name, const wa::signal::SenderKeyState &state) override {
        q->m_senderKeyJson[QString::fromStdString(name)] =
            QString::fromStdString(wa::signal::serializeSenderKeyState(state));
        q->saveCredentials();
    }

private:
    WhatsAppDirect *q;
};

WhatsAppDirect::WhatsAppDirect(QObject *parent) : QObject(parent) {
    m_keepAlive.setInterval(30000);   // keepAliveIntervalMs
    connect(&m_keepAlive, SIGNAL(timeout()), this, SLOT(sendKeepAlive()));

    m_reconnectTimer.setSingleShot(true);
    SignalHook::attach(&m_reconnectTimer, SIGNAL(timeout()), [this]() {
        if (m_manualDisconnect) return;
        // Baileys: reconnect after 5s unless the session was logged out.
        m_reconnectAttempts++;
        emit connectionUpdate("connecting",
                              QString("reconnect #%1").arg(m_reconnectAttempts));
        connectToWhatsApp();
    }, false);

    connect(&m_socket, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(&m_socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(&m_socket, SIGNAL(binaryMessageReceived(QByteArray)), this,
            SLOT(onBinaryMessage(QByteArray)));
    connect(&m_socket, SIGNAL(error(QString)), this, SLOT(onSocketError(QString)));

    m_net = new QNetworkAccessManager(this);
    m_store.reset(new CredsStore(this));
    m_senderKeys.reset(new SenderKeysStore(this));

    m_credsPath = QDir::homePath() + "/.openknot-qt/creds.json";
}

WhatsAppDirect::~WhatsAppDirect() = default;

void WhatsAppDirect::setCredentialsPath(const QString &path) { m_credsPath = path; }

void WhatsAppDirect::setBrowser(const QString &os, const QString &browser) {
    m_browserOs = os;
    m_browserName = browser;
}

// ─── Credentials ────────────────────────────────────────────────────────────

QByteArray WhatsAppDirect::randomBytes32() {
    const Bytes b = wa::crypto::randomBytes(32);
    return toQ(b);
}

void WhatsAppDirect::generateNewCredentials() {
    // structs from Baileys auth-utils: noiseKey (Curve25519 X25519),
    // signedIdentityKey (same curve), signedPreKey (curve key + signature),
    // registrationId (uint14-ish random), advSecretKey (32 random bytes).
    const wa::crypto::CurveKeyPair noise = wa::crypto::generateCurveKeyPair();
    m_noisePriv = toQ(noise.priv);
    m_noisePub = toQ(noise.pub);

    const wa::crypto::CurveKeyPair identity = wa::crypto::generateCurveKeyPair();
    m_identityPriv = toQ(identity.priv);
    m_identityPub = toQ(identity.pub);

    const wa::crypto::CurveKeyPair preKey = wa::crypto::generateCurveKeyPair();
    m_signedPreKeyPriv = toQ(preKey.priv);
    m_signedPreKeyPub = toQ(preKey.pub);
    // The server (and the peer) verify an XEdDSA signature over the *prefixed*
    // 33-byte signed pre-key, exactly like libsignal's generateSignedPreKey.
    Bytes prefixedSignedKey = toStd(m_signedPreKeyPub);
    prefixedSignedKey.insert(prefixedSignedKey.begin(), 0x05);
    m_signedPreKeySig = toQ(wa::crypto::xeddsaSign(toStd(m_identityPriv), prefixedSignedKey));
    m_signedPreKeyId = 1;

    // Baileys: a random 14-bit registration id.
    const wa::crypto::Bytes regBytes = wa::crypto::randomBytes(4);
    m_registrationId = (quint32(regBytes[0]) << 8 | quint32(regBytes[1])) % 0x3fffeu + 1;
    m_advSecretKey = randomBytes32();
    m_meJid.clear();
    m_hasMe = false;
    m_preKeys = QJsonArray();
    m_sessions = QJsonObject();
    m_senderKeyJson = QJsonObject();
    m_nextPreKeyId = 1;
    m_preKeysUploaded = false;
    ensurePreKeys(30);   // INITIAL_PREKEY_COUNT
    saveCredentials();
}

bool WhatsAppDirect::loadCredentials() {
    QFile f(m_credsPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_noisePriv = fromB64(o.value("noisePriv").toString());
    m_noisePub = fromB64(o.value("noisePub").toString());
    m_identityPriv = fromB64(o.value("identityPriv").toString());
    m_identityPub = fromB64(o.value("identityPub").toString());
    m_signedPreKeyPriv = fromB64(o.value("signedPreKeyPriv").toString());
    m_signedPreKeyPub = fromB64(o.value("signedPreKeyPub").toString());
    m_signedPreKeySig = fromB64(o.value("signedPreKeySig").toString());
    m_signedPreKeyId = quint32(o.value("signedPreKeyId").toInt(1));
    m_registrationId = quint32(o.value("registrationId").toInt(0));
    m_advSecretKey = fromB64(o.value("advSecretKey").toString());
    m_meJid = o.value("meJid").toString();
    m_hasMe = !m_meJid.isEmpty();
    m_preKeys = o.value("preKeys").toArray();
    m_sessions = o.value("sessions").toObject();
    m_senderKeyJson = o.value("senderKeys").toObject();
    m_nextPreKeyId = quint32(o.value("nextPreKeyId").toInt(1));
    m_lastUploadedPreKeyId = quint32(o.value("lastUploadedPreKeyId").toInt(0));
    m_preKeysUploaded = o.value("preKeysUploaded").toBool(false);

    if (o.contains("appVersion")) {
        const QJsonObject v = o.value("appVersion").toObject();
        m_appVersionPrimary = quint32(v.value("primary").toInt(2));
        m_appVersionSecondary = quint32(v.value("secondary").toInt(3000));
        m_appVersionTertiary = quint32(v.value("tertiary").toInt(1035939187));
    }
    return m_noisePriv.size() == 32 && m_noisePub.size() == 32;
}

bool WhatsAppDirect::saveCredentials() {
    QDir().mkpath(QFileInfo(m_credsPath).absolutePath());
    QFile f(m_credsPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logLine("[direct] cannot write credentials: " + m_credsPath);
        return false;
    }
    QJsonObject o;
    o["noisePriv"] = b64(m_noisePriv);
    o["noisePub"] = b64(m_noisePub);
    o["identityPriv"] = b64(m_identityPriv);
    o["identityPub"] = b64(m_identityPub);
    o["signedPreKeyPriv"] = b64(m_signedPreKeyPriv);
    o["signedPreKeyPub"] = b64(m_signedPreKeyPub);
    o["signedPreKeySig"] = b64(m_signedPreKeySig);
    o["signedPreKeyId"] = int(m_signedPreKeyId);
    o["registrationId"] = int(m_registrationId);
    o["advSecretKey"] = b64(m_advSecretKey);
    o["meJid"] = m_meJid;
    o["preKeys"] = m_preKeys;
    o["sessions"] = m_sessions;
    o["senderKeys"] = m_senderKeyJson;
    o["nextPreKeyId"] = int(m_nextPreKeyId);
    o["lastUploadedPreKeyId"] = int(m_lastUploadedPreKeyId);
    o["preKeysUploaded"] = m_preKeysUploaded;
    QJsonObject v;
    v["primary"] = int(m_appVersionPrimary);
    v["secondary"] = int(m_appVersionSecondary);
    v["tertiary"] = int(m_appVersionTertiary);
    o["appVersion"] = v;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

// ─── Connection lifecycle ───────────────────────────────────────────────────

void WhatsAppDirect::connectToWhatsApp() {
    m_manualDisconnect = false;
    m_handshakeDone = false;

    if (!loadCredentials()) {
        DLOG(this, "no credentials found - generating a new device identity");
        generateNewCredentials();
    }

    // WhatsApp requires the browser Origin and a desktop User-Agent; both go
    // into the HTTP upgrade request of the transport.
    const QString userAgent =
        QString("Mozilla/5.0 (%1) AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120.0.0.0 Safari/537.36").arg(m_browserOs);

    emit connectionUpdate("connecting", "opening websocket");
    DLOG(this, "connecting to " + QString::fromLatin1(kWebSocketUrl));
    m_socket.open(QUrl(QString::fromLatin1(kWebSocketUrl)), kOrigin, userAgent.toUtf8());
}

void WhatsAppDirect::disconnectFromWhatsApp() {
    m_manualDisconnect = true;
    m_keepAlive.stop();
    m_reconnectTimer.stop();
    m_socket.close();
}

void WhatsAppDirect::logout() {
    QFile::remove(m_credsPath);
    m_noisePriv.clear();
    m_noisePub.clear();
    m_meJid.clear();
    m_hasMe = false;
    disconnectFromWhatsApp();
    emit loggedOut();
}

void WhatsAppDirect::requestPairingQr() {
    // Asking again makes the server emit a new <pair-device> stanza.
    BinaryNode iq("iq");
    iq.setAttr("id", QString("qr-%1").arg(QDateTime::currentMSecsSinceEpoch()).toStdString());
    iq.setAttr("type", "get");
    iq.setAttr("to", "s.whatsapp.net");
    iq.setAttr("xmlns", "md");
    iq.addChild(BinaryNode("pair-device"));
    sendNode(iq);
}

void WhatsAppDirect::onConnected() {
    m_connected = true;
    m_reconnectAttempts = 0;
    emit connectionUpdate("connected", "websocket open");

    sendClientHello();   // builds a fresh Noise handler with a new ephemeral key
}

void WhatsAppDirect::onDisconnected() {
    const bool wasConnected = m_connected;
    m_connected = false;
    m_handshakeDone = false;
    m_keepAlive.stop();
    if (wasConnected) emit connectionUpdate("close", m_manualDisconnect ? "manual" : "socket closed");

    if (!m_manualDisconnect) {
        DLOG(this, "disconnected - reconnecting in 5s");
        m_reconnectTimer.start(5000);
    }
}

void WhatsAppDirect::onSocketError(const QString &reason) {
    emit connectionUpdate("error", reason);
    DLOG(this, "socket error: " + reason);
}

// ─── Handshake ─────────────────────────────────────────────────────────────

void WhatsAppDirect::sendClientHello() {
    // Baileys generates a fresh ephemeral pair per socket and builds the Noise
    // handler on it; the long-lived creds noiseKey is only used in ClientFinish.
    const wa::crypto::CurveKeyPair eph = wa::crypto::generateCurveKeyPair();
    m_ephPriv = toQ(eph.priv);
    m_ephPub = toQ(eph.pub);
    // std::make_unique is C++14; the Symbian toolchain stops earlier.
    m_noise.reset(new wa::noise::NoiseHandler(eph.priv, eph.pub));

    wa::proto::ClientHello hello;
    hello.ephemeral = eph.pub;

    const Bytes encoded = wa::proto::encodeClientHello(hello);
    const Bytes frame = m_noise->encodeFrame(encoded);
    DLOG(this, QString("ClientHello sent (%1 bytes)").arg(frame.size()));
    m_socket.sendBinaryMessage(toQ(frame));
}

bool WhatsAppDirect::handleServerHello(const QByteArray &frame) {
    // Server frames have no WA header; they start with the 3-byte length prefix.
    if (frame.size() < 4) return false;
    const int len = (quint8(frame[0]) << 16) | (quint8(frame[1]) << 8) | quint8(frame[2]);
    if (len <= 0 || frame.size() < 3 + len) return false;

    const QByteArray payload = frame.mid(3, len);
    wa::proto::ServerHello hello;
    if (!wa::proto::decodeServerHello(toStd(payload), hello)) {
        streamError("handshake", "could not decode ServerHello");
        return false;
    }

    uint64_t serial = 0;
    bool certValid = false;
    Bytes keyEnc;
    try {
        keyEnc = m_noise->processHandshake(hello, toStd(m_noisePriv), toStd(m_noisePub),
                                           &serial, &certValid);
    } catch (const std::exception &e) {
        streamError("handshake", QString::fromUtf8(e.what()));
        return false;
    }
    if (!certValid) {
        DLOG(this, QString("warning: cert chain serial %1 could not be verified "
                           "(needs WAProto CertChain decoding)").arg(qulonglong(serial)));
    }
    m_keyEnc = toQ(keyEnc);
    DLOG(this, "ServerHello processed - Noise keys mixed");
    return sendClientFinish();
}

bool WhatsAppDirect::sendClientFinish() {
    wa::proto::ClientPayload payload;
    payload.connectType = 1;      // WIFI_UNKNOWN
    payload.connectReason = 1;    // USER_ACTIVATED
    payload.hasUserAgent = true;
    payload.userAgent.platform = 1;   // WEB
    payload.userAgent.appVersion.primary = m_appVersionPrimary;
    payload.userAgent.appVersion.secondary = m_appVersionSecondary;
    payload.userAgent.appVersion.tertiary = m_appVersionTertiary;
    payload.userAgent.localeCountryIso31661Alpha2 = "US";
    payload.webSubPlatform = 0;       // WEB_BROWSER
    payload.hasPassive = true;
    payload.passive = false;

    if (!m_hasMe) {
        // Registration (QR pairing) payload, mirroring generateRegistrationNode.
        payload.hasPull = true;
        payload.pull = false;
        payload.hasDevicePairingData = true;

        auto &d = payload.devicePairingData;
        d.eRegid = wa::proto::encodeBigEndian(m_registrationId);
        d.eKeytype = Bytes{5};   // KEY_BUNDLE_TYPE
        d.eIdent = toStd(m_identityPub);
        d.eSkeyId = wa::proto::encodeBigEndian(m_signedPreKeyId, 3);
        d.eSkeyVal = toStd(m_signedPreKeyPub);
        d.eSkeySig = toStd(m_signedPreKeySig);
        const std::string versionStr = QString("%1.%2.%3")
                                           .arg(m_appVersionPrimary)
                                           .arg(m_appVersionSecondary)
                                           .arg(m_appVersionTertiary)
                                           .toStdString();
        d.buildHash = wa::proto::md5(Bytes(versionStr.begin(), versionStr.end()));
        d.deviceProps = wa::proto::encodeDeviceProps(
            m_browserOs.toStdString(), 15 /* CHROME platform type */, m_syncFullHistory,
            wa::proto::HistorySyncConfig());
    } else {
        // Login payload, mirroring generateLoginNode.
        const wa::Jid me = wa::jidDecode(m_meJid.toStdString());
        payload.username = QString::fromStdString(me.user).toULongLong();
        payload.hasDevice = true;
        payload.device = quint32(me.device > 0 ? me.device : 0);
        payload.hasPull = true;
        payload.pull = true;
    }

    const Bytes plain = wa::proto::encodeClientPayload(payload);
    const Bytes encPayload = m_noise->encrypt(plain);

    wa::proto::ClientFinish finish;
    finish.staticKey = toStd(m_keyEnc);
    finish.payload = encPayload;
    const Bytes finishBytes = wa::proto::encodeClientFinish(finish);
    const Bytes frame = m_noise->encodeFrame(finishBytes);
    m_socket.sendBinaryMessage(toQ(frame));

    m_noise->finishInit();
    m_handshakeDone = true;
    emit transportReady();
    startKeepAlive();
    DLOG(this, QString("ClientFinish sent (%1) - Noise handshake complete")
                   .arg(m_hasMe ? "login" : "registration"));
    return true;
}

void WhatsAppDirect::startKeepAlive() { m_keepAlive.start(); }

void WhatsAppDirect::sendKeepAlive() {
    if (!m_connected || !m_handshakeDone) return;
    BinaryNode iq("iq");
    iq.setAttr("id", QString("ping-%1").arg(QDateTime::currentMSecsSinceEpoch()).toStdString());
    iq.setAttr("type", "get");
    iq.setAttr("to", "s.whatsapp.net");
    iq.setAttr("xmlns", "urn:xmpp:ping");
    sendNode(iq);
}

// ─── Stanza plumbing ───────────────────────────────────────────────────────

void WhatsAppDirect::onBinaryMessage(const QByteArray &data) {
    if (!m_noise) return;

    if (!m_handshakeDone) {
        handleServerHello(data);
        return;
    }

    m_noise->decodeFrames(toStd(data), [this](const Bytes &frame) {
        if (frame.empty()) {
            streamError("decrypt", "frame authentication failed");
            return;
        }
        auto res = wa::binary::decode(frame);
        if (!res.ok) {
            DLOG(this, "failed to decode frame: " + QString::fromStdString(res.error));
            return;
        }
        handleNode(res.node);
    });
}

void WhatsAppDirect::sendNode(const BinaryNode &node) {
    if (!m_noise || !m_connected) return;
    const Bytes encoded = wa::binary::encode(node);
    const Bytes frame = m_noise->encodeFrame(encoded);
    m_socket.sendBinaryMessage(toQ(frame));
}

void WhatsAppDirect::handleNode(const BinaryNode &node) {
    const QString tag = QString::fromStdString(node.tag);

    QVariantMap attrs;
    for (const auto &kv : node.attrs) {
        attrs[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
    }
    emit nodeReceived(tag, attrs);

    if (tag == "success") { handleSuccess(node); return; }
    if (tag == "failure") {
        const QString reason = attrs.value("reason").toString();
        emit connectionUpdate("close", "failure: " + reason);
        if (reason == "401" || attrs.value("status").toString() == "401") {
            // Credentials no longer valid: Baileys treats 401/403/419 as logout.
            QFile::remove(m_credsPath);
            m_manualDisconnect = true;
            emit loggedOut();
        }
        return;
    }
    if (tag == "stream:error") {
        const BinaryNode *code = node.child("code");
        emit streamError(code ? QString::fromStdString(code->text) : "unknown",
                         QString::fromStdString(node.attrOr("reason")));
        return;
    }
    if (tag == "iq") {
        // Answers to our <iq xmlns=encrypt><key/></iq> key requests.
        const QString iqId = QString::fromStdString(node.attrOr("id"));
        const QString iqType = attrs.value("type").toString();
        // Answers to group administration IQs (create/join/leave/participants/
        // subject/description/invite).
        if (m_pendingGroupOps.contains(iqId)) {
            handleGroupOpResult(iqId, node);
            return;
        }
        if (m_pendingSends.contains(iqId) && (iqType == "result" || iqType == "error")) {
            finishSend(iqId, node);
            return;
        }
        // Answers to <iq xmlns=w:m><media_conn/></iq>.
        if (node.child("media_conn") || (iqType == "error" &&
                                         m_waitingForMediaConn.contains(iqId))) {
            wa::media::MediaConn conn;
            std::string mediaError;
            if (!wa::media::parseMediaConn(node, conn, mediaError)) {
                DLOG(this, "media_conn failed: " + QString::fromStdString(mediaError));
                if (m_waitingForMediaConn.contains(iqId)) {
                    const PendingSend waiting = m_waitingForMediaConn.take(iqId);
                    if (m_pendingUploads.contains(waiting.msgId)) {
                        m_pendingUploads.remove(waiting.msgId);
                        emit sendResult(waiting.msgId, false,
                                        QString::fromStdString(mediaError));
                    }
                }
                return;
            }
            conn.fetchedAt = qt_now_secs();
            m_mediaConn = conn;
            DLOG(this, QString("media connection ready (host %1, ttl %2s)")
                           .arg(QString::fromStdString(conn.hostname))
                           .arg(conn.ttlSeconds));
            const PendingSend waiting = m_waitingForMediaConn.take(iqId);
            if (!waiting.msgId.isEmpty()) startMediaUpload(waiting);
            // Any other upload queued while we were waiting.
            for (auto it = m_waitingForMediaConn.begin();
                 it != m_waitingForMediaConn.end();) {
                const PendingSend queued = it.value();
                it = m_waitingForMediaConn.erase(it);
                startMediaUpload(queued);
            }
            return;
        }
        // Answers to our <iq xmlns=w:g2><query/></iq> group metadata requests.
        if (m_pendingGroupMeta.contains(iqId) && (iqType == "result" || iqType == "error")) {
            const PendingSend pending = m_pendingGroupMeta.take(iqId);
            wa::messaging::GroupInfo info;
            std::string error;
            if (!wa::messaging::parseGroupMetadata(node, info, error)) {
                emit sendResult(pending.msgId, false, QString::fromStdString(error));
                DLOG(this, "group metadata failed for " + pending.jid + ": " +
                               QString::fromStdString(error));
                return;
            }
            if (info.subject.empty())
                info.subject = pending.jid.toStdString();
            m_groupCache.insert(pending.jid, info);
            Chat chat = chatModelFor(info.jid);
            chat.name = QString::fromStdString(info.subject);
            chat.isGroup = true;
            emit chatUpdated(chat);
            fetchGroupBundles(pending, info);
            return;
        }
        // <pair-device> arrives as an iq with type=set.
        if (const BinaryNode *pair = node.child("pair-device")) {
            handlePairDevice(*pair);
        } else if (attrs.value("type").toString() == "get" &&
                   !attrs.value("xmlns").toString().isEmpty()) {
            replyToIq(node, "result");   // ack unknown server queries
        }
        return;
    }
    if (tag == "message") { handleEncryptedMessage(node); return; }
    if (tag == "receipt") { handleReceipt(node); return; }
    if (tag == "presence" || tag == "chatstate" || tag == "presence-update") {
        handlePresence(node);
        return;
    }
    if (tag == "notification") {
        const QString noteType = attrs.value("type").toString();
        // Group changes arrive as <notification type="w:gp2" from=group>.
        if (noteType == "w:gp2") handleGroupNotification(node);
        // The server asks for more one-time pre-keys when it is running low.
        else if (noteType == "encrypt") {
            const wa::BinaryNode *count = node.child("count");
            const int left = count ? std::atoi(count->attrOr("value", "0").c_str()) : 0;
            DLOG(this, QString("server wants more pre-keys (%1 left)").arg(left));
            if (m_meJid.size()) uploadPreKeys();
        }
        return;
    }
    if (tag == "ib") {   // history sync notification (needs WAProto)
        emit historySyncReceived(0, 0, 0);
        return;
    }
    if (tag == "ack") { return; }
    if (tag == "xmlstreamend") {
        emit connectionUpdate("close", "server closed the stream");
        return;
    }
}

void WhatsAppDirect::handlePairDevice(const BinaryNode &pairDevice) {
    // Baileys takes the *last* <ref> child as the QR string.
    const auto refs = pairDevice.childrenNamed("ref");
    if (refs.empty()) return;
    const QString qr = QString::fromStdString(refs.back()->text);
    DLOG(this, "pair-device received: QR ref (waiting for scan)");
    emit qrCode(qr);
}

void WhatsAppDirect::handleSuccess(const BinaryNode &success) {
    // After pairing the server sends <success> with our JID (device attr).
    const QString lid = QString::fromStdString(success.attrOr("lid"));
    QString jid = QString::fromStdString(success.attrOr("jid"));
    if (jid.isEmpty() && !lid.isEmpty()) jid = lid;
    if (!jid.isEmpty()) {
        m_meJid = jid;
        m_hasMe = true;
        saveCredentials();
        emit paired(jid);
    }
    emit connectionUpdate("open", "logged in");
    DLOG(this, "connection established as " + m_meJid);

    // Publish one-time pre-keys so peers can open sessions with this device.
    if (m_preKeys.size() < 5) ensurePreKeys(30);
    uploadPreKeys();
}

void WhatsAppDirect::replyToIq(const BinaryNode &iq, const QString &resultTag) {
    BinaryNode reply(resultTag.toStdString());
    reply.setAttr("id", iq.attrOr("id"));
    reply.setAttr("type", "result");
    reply.setAttr("to", iq.attrOr("from", "s.whatsapp.net"));
    sendNode(reply);
}

// ─── Message decryption (Signal + WAProto) ─────────────────────────────────

void WhatsAppDirect::handleEncryptedMessage(const BinaryNode &node) {
    const std::string meJid = m_meJid.toStdString();
    wa::messaging::IncomingMessage msg;
    std::string error;

    if (!wa::messaging::decryptMessageNode(*m_store, m_senderKeys.get(), node, meJid, msg,
                                          error)) {
        // Baileys sends a retry receipt so the sender re-encrypts with a fresh
        // pre-key message; we log instead (see QT_PORT.md §6).
        DLOG(this, "undecryptable message " + QString::fromStdString(msg.id) + ": " +
                       QString::fromStdString(error));
        emit messageStatus(QString::fromStdString(msg.chatJid),
                           QString::fromStdString(msg.id), "undecryptable");
        return;
    }

    DLOG(this, "decrypted message " + QString::fromStdString(msg.id) + " in " +
                   QString::fromStdString(msg.chatJid));
    emit messageStatus(QString::fromStdString(msg.chatJid),
                       QString::fromStdString(msg.id), "received");
    if (!msg.fromMe) sendReceipt(node);   // our own echoes are not acknowledged

    if (msg.hasHistorySync) {
        downloadHistory(msg.historySync);
        return;   // protocol messages are not shown in the chat
    }

    // Keep the CDN metadata of received media so downloadMedia() can fetch it
    // when the user asks (the message itself only carries the keys).
    wa::media::Type mediaType = wa::media::Type::Image;
    wa::waproto::MediaInfo mediaInfo;
    if (wa::media::mediaInfoOf(msg.message, mediaType, mediaInfo))
        registerMedia(QString::fromStdString(msg.id), mediaType, mediaInfo);

    const Message model = messageModelFor(msg);
    emit messageAdded(model);

    QJsonObject json;
    json["id"] = model.id;
    json["chatJid"] = model.chatJid;
    json["fromMe"] = model.fromMe;
    json["senderJid"] = model.senderJid;
    json["pushName"] = QString::fromStdString(msg.pushName);
    json["text"] = model.text;
    json["timestamp"] = model.timestamp;
    json["isGroup"] = msg.isGroup;
    emit incomingMessage(json);
}

void WhatsAppDirect::sendReceipt(const BinaryNode &node) {
    const std::string id = node.attrOr("id");
    const std::string from = node.attrOr("from");
    if (id.empty() || from.empty()) return;

    BinaryNode receipt("receipt");
    receipt.setAttr("id", id);
    receipt.setAttr("to", from);
    const std::string participant = node.attrOr("participant");
    if (!participant.empty()) receipt.setAttr("participant", participant);
    sendNode(receipt);
}

Chat WhatsAppDirect::chatModelFor(const std::string &jid) const {
    Chat chat;
    chat.jid = QString::fromStdString(jid);
    chat.id = chat.jid;
    chat.isGroup = wa::isJidGroup(jid);
    for (const auto &c : m_history.chats) {
        if (c.jid != jid) continue;
        chat.name = QString::fromStdString(c.name);
        chat.unreadCount = int(c.unreadCount);
        chat.lastMessageTs = qint64(c.lastMessageTimestamp);
        if (!c.messages.empty()) {
            chat.lastMessage = QString::fromStdString(c.messages.back().message.text());
        }
        break;
    }
    return chat;
}

Message WhatsAppDirect::messageModelFor(const wa::messaging::IncomingMessage &in) const {
    Message m;
    m.id = QString::fromStdString(in.id);
    m.chatJid = QString::fromStdString(in.chatJid);
    m.fromMe = in.fromMe;
    m.senderJid = QString::fromStdString(in.senderJid);
    m.senderName = QString::fromStdString(in.pushName);
    m.timestamp = qint64(in.timestamp);
    m.text = QString::fromStdString(in.text);
    m.mediaType = QString::fromStdString(in.message.mediaType());
    m.hasMedia = !m.mediaType.isEmpty();
    m.status = in.fromMe ? QStringLiteral("sent") : QStringLiteral("received");
    return m;
}

// ─── History sync ──────────────────────────────────────────────────────────

void WhatsAppDirect::downloadHistory(const wa::waproto::HistorySyncNotification &note) {
    const QString url =
        QString::fromStdString(wa::media::urlFromDirectPath(note.directPath));
    if (url.isEmpty() || note.mediaKey.size() != 32) {
        DLOG(this, "history sync notification without a usable directPath/mediaKey");
        return;
    }
    DLOG(this, QString("downloading history chunk %1 (%2%%)")
                   .arg(note.chunkOrder)
                   .arg(note.progress));

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Origin", "https://web.whatsapp.net");
    QNetworkReply *reply = m_net->get(req);
    const wa::waproto::HistorySyncNotification carried = note;
    SignalHook::onFinished(reply, [this, reply, carried]() {
        const QByteArray data = reply->readAll();
        const QString netError = reply->error() == QNetworkReply::NoError
                                     ? QString()
                                     : reply->errorString();
        reply->deleteLater();
        if (!netError.isEmpty()) {
            DLOG(this, "history download failed: " + netError);
            return;
        }

        wa::history::SyncData sync;
        std::string error;
        if (!wa::history::decode(toStd(data), carried.mediaKey, sync, error)) {
            DLOG(this, "history decode failed: " + QString::fromStdString(error));
            return;
        }

        m_history.merge(sync);
        for (const auto &chat : m_history.chats) emit chatUpdated(chatModelFor(chat.jid));
        const int chats = int(m_history.chats.size());
        const int messages = int(m_history.messageCount());
        DLOG(this, QString("history chunk %1 applied: %2 chats, %3 messages")
                       .arg(carried.chunkOrder)
                       .arg(chats)
                       .arg(messages));
        emit historySyncReceived(carried.progress ? int(carried.progress) : 100, chats,
                                 messages);
    });
}

void WhatsAppDirect::handleReceipt(const BinaryNode &node) {
    const QString chat = QString::fromStdString(node.attrOr("from"));
    const auto ids = node.childrenNamed("message");
    for (const BinaryNode *m : ids) {
        const QString msgId = QString::fromStdString(m->attrOr("id"));
        emit messageStatus(chat, msgId, QString::fromStdString(node.attrOr("type", "delivered")));
    }
    if (ids.empty()) {
        const QString msgId = QString::fromStdString(node.attrOr("id"));
        if (!msgId.isEmpty())
            emit messageStatus(chat, msgId, QString::fromStdString(node.attrOr("type", "delivered")));
    }
}

void WhatsAppDirect::handlePresence(const BinaryNode &node) {
    const QString jid = QString::fromStdString(node.attrOr("from"));
    QString presence = QString::fromStdString(node.attrOr("type"));
    if (presence.isEmpty()) {
        if (const BinaryNode *c = node.child("composing")) presence = "composing";
        else if (node.child("paused")) presence = "paused";
    }
    if (!jid.isEmpty() && !presence.isEmpty()) emit presenceUpdate(jid, presence);
}

// ─── Sending ───────────────────────────────────────────────────────────────

void WhatsAppDirect::sendText(const QString &jid, const QString &text) {
    if (!m_connected || !m_handshakeDone || m_meJid.isEmpty()) {
        emit sendResult(QString(), false, "not connected to WhatsApp");
        return;
    }
    if (jid.isEmpty()) {
        emit sendResult(QString(), false, "no chat selected");
        return;
    }
    PendingSend pending;
    pending.jid = jid;
    pending.text = text;
    pending.msgId = QString::fromStdString(wa::messaging::generateMessageId());
    enqueueSend(pending);
}

// Groups need metadata, 1:1 needs device bundles; both are cached, so this is
// where a send actually starts (text and media share it).
void WhatsAppDirect::enqueueSend(const PendingSend &incoming) {
    PendingSend pending = incoming;
    pending.isGroup = wa::isJidGroup(pending.jid.toStdString());

    if (pending.isGroup) {
        const auto cached = m_groupCache.constFind(pending.jid);
        if (cached != m_groupCache.constEnd() && !cached->participants.empty()) {
            fetchGroupBundles(pending, *cached);
        } else {
            requestGroupMetadata(pending);
        }
        return;
    }

    // Device bundles are cached for a few minutes (Baileys' USER_DEVICES TTL).
    const QString userJid =
        QString::fromStdString(wa::jidNormalizedUser(pending.jid.toStdString()));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cached = m_bundleCache.constFind(userJid);
    if (cached != m_bundleCache.constEnd() && now - cached->fetchedAt < 5 * 60 * 1000 &&
        !cached->devices.empty()) {
        sendWithBundles(pending, cached->devices);
        return;
    }

    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    m_pendingSends.insert(iqId, pending);

    const std::vector<std::string> request{pending.jid.toStdString(),
                                          m_meJid.toStdString()};
    DLOG(this, "fetching device bundles for " + pending.jid + "/" + m_meJid);
    sendNode(wa::messaging::buildKeyRequest(iqId.toStdString(), request));
}

// ─── Media (CDN upload/download) ───────────────────────────────────────────

void WhatsAppDirect::registerMedia(const QString &msgId, wa::media::Type type,
                                   const wa::waproto::MediaInfo &info) {
    m_mediaIndex.insert(msgId, qMakePair(int(type), info));
}

void WhatsAppDirect::sendMedia(const QString &jid, const QString &filePath,
                               const QString &caption, bool voiceNote) {
    if (!m_connected || !m_handshakeDone || m_meJid.isEmpty()) {
        emit sendResult(QString(), false, "not connected to WhatsApp");
        return;
    }
    if (jid.isEmpty()) {
        emit sendResult(QString(), false, "no chat selected");
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit sendResult(QString(), false, "cannot read " + filePath);
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();
    if (raw.isEmpty()) {
        emit sendResult(QString(), false, "file is empty: " + filePath);
        return;
    }

    const QFileInfo info(filePath);
    const std::string fileName = info.fileName().toStdString();
    PendingUpload upload;
    upload.send.jid = jid;
    upload.send.msgId = QString::fromStdString(wa::messaging::generateMessageId());
    upload.meta.fileName = fileName;
    upload.meta.mimetype = wa::media::mimeForFileName(fileName);
    upload.meta.caption = caption.toStdString();
    upload.meta.title = fileName;
    upload.type = wa::media::typeForMime(upload.meta.mimetype, fileName);

    if (voiceNote) {
        if (upload.type != wa::media::Type::Audio) {
            emit sendResult(upload.send.msgId, false,
                            "voice notes must be audio (ogg/opus)");
            return;
        }
        upload.meta.ptt = true;
        int seconds = 0;
        if (wa::media::oggOpusDuration(wa::media::Bytes(raw.begin(), raw.end()), seconds))
            upload.meta.seconds = seconds;
        // WhatsApp tags voice notes as opus/ogg regardless of container.
        upload.meta.mimetype = "audio/ogg; codecs=opus";
    }

    // A fresh media key per file, exactly like Baileys' encryptedStream().
    const QByteArray mediaKey = randomBytes32();
    const wa::media::Bytes key(mediaKey.begin(), mediaKey.end());
    std::string error;
    if (!wa::media::prepareUpload(toStd(raw), upload.type, key,
                                  qt_now_secs(), upload.uploaded,
                                  error)) {
        emit sendResult(upload.send.msgId, false, QString::fromStdString(error));
        return;
    }

    const QString uploadId = upload.send.msgId;
    m_pendingUploads.insert(uploadId, upload);
    emit mediaProgress(uploadId, "encrypted");
    DLOG(this, QString("prepared %1 for upload (%2 bytes encrypted)")
                   .arg(QString::fromStdString(fileName))
                   .arg(int(upload.uploaded.blob.size())));

    if (m_mediaConn.usable(qt_now_secs())) {
        PendingSend ready;
        ready.jid = jid;
        ready.msgId = uploadId;
        startMediaUpload(ready);
        return;
    }

    // Ask the server where to upload; the reply restarts the upload.
    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    PendingSend waiting;
    waiting.jid = jid;
    waiting.msgId = uploadId;
    waiting.isMedia = true;
    m_waitingForMediaConn.insert(iqId, waiting);
    DLOG(this, "requesting media connection");
    sendNode(wa::media::buildMediaConnRequest(iqId.toStdString()));
}

void WhatsAppDirect::startMediaUpload(const PendingSend &pending) {
    if (!m_pendingUploads.contains(pending.msgId)) return;
    const PendingUpload &upload = m_pendingUploads[pending.msgId];

    QNetworkRequest request(
        QUrl(QString::fromStdString(wa::media::uploadUrl(
            m_mediaConn, upload.type, upload.uploaded.fileEncSha256))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setRawHeader("Origin", kOrigin);

    const QString uploadId = pending.msgId;
    QNetworkReply *reply = m_net->post(
        request, QByteArray(reinterpret_cast<const char *>(upload.uploaded.blob.data()),
                            int(upload.uploaded.blob.size())));
    emit mediaProgress(uploadId, "uploading");
    DLOG(this, QString("uploading %1 (%2 encrypted bytes) to the CDN")
                   .arg(uploadId)
                   .arg(int(upload.uploaded.blob.size())));

    SignalHook::onFinished(reply, [this, reply, uploadId]() {
        const QByteArray body = reply->readAll();
        const QString errorText =
            reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        finishMediaUpload(uploadId, body, errorText);
    });
}

void WhatsAppDirect::finishMediaUpload(const QString &uploadId, const QByteArray &body,
                                       const QString &errorText) {
    if (!m_pendingUploads.contains(uploadId)) return;

    if (!errorText.isEmpty()) {
        const PendingUpload failed = m_pendingUploads.take(uploadId);
        const QString reason = errorText + " " + QString::fromUtf8(body.left(200));
        emit sendResult(failed.send.msgId, false, "upload failed: " + reason);
        DLOG(this, "media upload failed: " + reason);
        m_mediaConn.fetchedAt = 0;   // force a fresh media_conn next time
        return;
    }

    const QJsonObject result = QJsonDocument::fromJson(body).object();
    const QString url = result.value("url").toString();
    const QString directPath = result.value("direct_path").toString();
    if (url.isEmpty() && directPath.isEmpty()) {
        const PendingUpload failed = m_pendingUploads.take(uploadId);
        emit sendResult(failed.send.msgId, false,
                        "upload response had no url: " + QString::fromUtf8(body.left(200)));
        DLOG(this, "media upload returned no url: " + QString::fromUtf8(body.left(200)));
        return;
    }

    PendingUpload upload = m_pendingUploads.take(uploadId);
    upload.uploaded.url = url.toStdString();
    upload.uploaded.directPath = directPath.toStdString();
    wa::media::fillMediaMessage(upload.type, upload.uploaded, upload.meta,
                                upload.send.payload);
    upload.send.isMedia = true;
    upload.send.text = QString::fromStdString(upload.send.payload.text());

    // Remember the CDN metadata so the sent bubble can be re-downloaded.
    wa::media::Type kind = upload.type;
    wa::waproto::MediaInfo mediaInfo;
    if (wa::media::mediaInfoOf(upload.send.payload, kind, mediaInfo))
        registerMedia(upload.send.msgId, kind, mediaInfo);

    emit mediaProgress(upload.send.msgId, "uploaded");
    DLOG(this, "media uploaded to " + (url.isEmpty() ? QString() : url));
    enqueueSend(upload.send);
}

bool WhatsAppDirect::downloadMedia(const QString &chatJid, const QString &msgId,
                                   const QString &savePath) {
    if (!m_mediaIndex.contains(msgId)) {
        emit mediaFailed(chatJid, msgId, "no media recorded for this message");
        return false;
    }
    const QPair<int, wa::waproto::MediaInfo> entry = m_mediaIndex.value(msgId);
    const wa::media::Bytes mediaKey = entry.second.mediaKey;
    const std::string url =
        wa::media::downloadUrl(entry.second.url, entry.second.directPath);
    if (url.empty()) {
        emit mediaFailed(chatJid, msgId, "message has no CDN url or directPath");
        return false;
    }

    QNetworkRequest request(QUrl(QString::fromStdString(url)));
    request.setRawHeader("Origin", kOrigin);
    QNetworkReply *reply = m_net->get(request);
    emit mediaProgress(msgId, "downloading");

    const int kind = entry.first;
    SignalHook::onFinished(reply, [this, reply, chatJid, msgId, savePath, mediaKey, kind]() {
                const QByteArray body = reply->readAll();
                const QString errorText = reply->error() == QNetworkReply::NoError
                                              ? QString()
                                              : reply->errorString();
                reply->deleteLater();
                if (!errorText.isEmpty()) {
                    emit mediaFailed(chatJid, msgId, errorText);
                    return;
                }
                wa::media::Bytes plaintext;
                std::string error;
                const wa::media::Bytes blob(
                    reinterpret_cast<const uint8_t *>(body.constData()),
                    reinterpret_cast<const uint8_t *>(body.constData()) + size_t(body.size()));
                if (!wa::media::decryptDownload(blob, mediaKey, wa::media::Type(kind),
                                                plaintext, error)) {
                    emit mediaFailed(chatJid, msgId, QString::fromStdString(error));
                    return;
                }
                QFile out(savePath);
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    emit mediaFailed(chatJid, msgId, "cannot write " + savePath);
                    return;
                }
                out.write(QByteArray(reinterpret_cast<const char *>(plaintext.data()),
                                     int(plaintext.size())));
                out.close();
                emit mediaSaved(chatJid, msgId, savePath);
            });
    return true;
}

// Server-side group change: <notification type="w:gp2" from=groupJid
// participant=…><subject subject="New"/></notification>. Keeps the chat list
// and the cached metadata in step with what other members do.
void WhatsAppDirect::handleGroupNotification(const wa::BinaryNode &node) {
    const std::string groupJid = node.attrOr("from");
    if (groupJid.empty() || !wa::isJidGroup(groupJid) || node.children.empty()) return;
    const wa::BinaryNode &child = node.children.front();
    const QString jid = QString::fromStdString(groupJid);
    const QString kind = QString::fromStdString(child.tag);
    QString detail;

    auto refreshChat = [&](const std::string &subject) {
        Chat chat = chatModelFor(groupJid);
        chat.name = QString::fromStdString(subject);
        chat.isGroup = true;
        emit chatUpdated(chat);
    };

    if (child.tag == "subject" || child.tag == "create") {
        detail = QString::fromStdString(child.attrOr("subject"));
        if (m_groupCache.contains(jid)) {
            wa::messaging::GroupInfo info = m_groupCache.value(jid);
            if (!detail.isEmpty()) info.subject = detail.toStdString();
            m_groupCache.insert(jid, info);
        }
        if (!detail.isEmpty()) refreshChat(detail.toStdString());
    } else if (child.tag == "description") {
        if (const wa::BinaryNode *body = child.child("body")) detail = QString::fromStdString(body->text);
        if (m_groupCache.contains(jid)) {
            wa::messaging::GroupInfo info = m_groupCache.value(jid);
            info.descId = child.attrOr("id");
            info.description = detail.toStdString();
            m_groupCache.insert(jid, info);
        }
    } else if (child.tag == "add" || child.tag == "remove" || child.tag == "promote" ||
               child.tag == "demote" || child.tag == "modify" || child.tag == "leave") {
        // Membership changed: the cached participant list is stale.
        m_groupCache.remove(jid);
        QStringList who;
        for (const wa::BinaryNode *p : child.childrenNamed("participant"))
            who << QString::fromStdString(p->attrOr("jid"));
        detail = who.join(", ");
        if (child.tag == "remove" && who.size() == 1 &&
            wa::areJidsSameUser(who.first().toStdString(),
                                QString::fromStdString(node.attrOr("participant")).toStdString())) {
            detail += " (left)";
        }
    }
    if (detail.isEmpty()) detail = QString::fromStdString(child.tag);
    DLOG(this, "group " + jid + " " + kind + ": " + detail);
    emit groupNotification(jid, kind, detail);
}

// ─── Group administration ──────────────────────────────────────────────────
//
// Every call sends one <iq xmlns=w:g2> stanza and parks a PendingGroupOp under
// its id; handleGroupOpResult() turns the reply into the group* signals. The
// node shapes come from wa_message.h (Baileys groups.js).

void WhatsAppDirect::sendGroupOp(const PendingGroupOp &op, const wa::BinaryNode &node) {
    if (!m_connected) {
        emit groupActionResult(op.kind, op.jid, false, "not connected");
        return;
    }
    m_pendingGroupOps.insert(QString::fromStdString(node.attrOr("id")), op);
    sendNode(node);
}

void WhatsAppDirect::createGroup(const QString &subject, const QStringList &participants) {
    std::vector<std::string> jids;
    for (const QString &jid : participants) {
        if (jid.trimmed().isEmpty()) continue;
        jids.push_back(jid.trimmed().toStdString());
    }
    if (subject.trimmed().isEmpty() || jids.empty()) {
        emit groupActionResult("create", QString(), false,
                               "a group needs a subject and at least one participant");
        return;
    }
    // Baileys sends a fresh key with <create>; it only has to be unique.
    const std::string id = wa::messaging::generateMessageId();
    PendingGroupOp op;
    op.kind = "create";
    op.detail = subject;
    sendGroupOp(op, wa::messaging::buildGroupCreateRequest(
                        id, subject.trimmed().toStdString(), jids, id));
    DLOG(this, QString("creating group \"%1\" with %2 participants")
                   .arg(subject.trimmed())
                   .arg(jids.size()));
}

void WhatsAppDirect::joinGroup(const QString &inviteCode) {
    QString code = inviteCode.trimmed();
    // Accept either a bare code or a https://chat.whatsapp.com/<code> link.
    const int slash = code.lastIndexOf('/');
    if (slash >= 0 && code.contains("chat.whatsapp.com")) code = code.mid(slash + 1);
    if (code.isEmpty()) {
        emit groupActionResult("join", QString(), false, "no invite code given");
        return;
    }
    PendingGroupOp op;
    op.kind = "join";
    op.detail = code;
    sendGroupOp(op, wa::messaging::buildGroupAcceptInviteRequest(
                        wa::messaging::generateMessageId(), code.toStdString()));
    DLOG(this, "joining group with invite code " + code);
}

void WhatsAppDirect::leaveGroup(const QString &groupJid) {
    if (groupJid.isEmpty()) return;
    PendingGroupOp op;
    op.kind = "leave";
    op.jid = groupJid;
    sendGroupOp(op, wa::messaging::buildGroupLeaveRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString()));
    DLOG(this, "leaving group " + groupJid);
}

void WhatsAppDirect::setGroupSubject(const QString &groupJid, const QString &subject) {
    if (groupJid.isEmpty() || subject.trimmed().isEmpty()) return;
    PendingGroupOp op;
    op.kind = "subject";
    op.jid = groupJid;
    op.detail = subject.trimmed();
    sendGroupOp(op, wa::messaging::buildGroupSubjectRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString(),
                        subject.trimmed().toStdString()));
    DLOG(this, "renaming group " + groupJid + " to " + subject.trimmed());
}

void WhatsAppDirect::setGroupDescription(const QString &groupJid, const QString &description) {
    if (groupJid.isEmpty()) return;
    // The server wants the *previous* description id to avoid clobbering a
    // concurrent edit; fetch metadata first when we have no cached copy.
    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    PendingGroupOp op;
    op.kind = "description";
    op.jid = groupJid;
    op.detail = description;
    if (!m_groupCache.contains(groupJid)) {
        // The server wants the previous description id, so fetch metadata first;
        // the reply ("description-meta") restarts this edit.
        PendingGroupOp meta;
        meta.kind = "description-meta";
        meta.jid = groupJid;
        meta.detail = description;
        sendGroupOp(meta, wa::messaging::buildGroupMetadataRequest(
                               iqId.toStdString(), groupJid.toStdString()));
        DLOG(this, "fetching group metadata before editing the description");
        return;
    }
    const wa::messaging::GroupInfo info = m_groupCache.value(groupJid);
    sendGroupOp(op, wa::messaging::buildGroupDescriptionRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString(),
                        description.toStdString(), info.descId,
                        description.isEmpty() ? std::string()
                                              : wa::messaging::generateMessageId()));
    DLOG(this, "updating the group description of " + groupJid);
}

void WhatsAppDirect::updateGroupParticipants(const QString &groupJid,
                                             const QStringList &participants,
                                             const QString &action) {
    wa::messaging::GroupAction parsed;
    if (!wa::messaging::parseGroupAction(action.toStdString(), parsed)) {
        emit groupActionResult(action, groupJid, false,
                               "unknown action (add/remove/promote/demote)");
        return;
    }
    std::vector<std::string> jids;
    for (const QString &jid : participants) {
        if (jid.trimmed().isEmpty()) continue;
        jids.push_back(jid.trimmed().toStdString());
    }
    if (jids.empty()) {
        emit groupActionResult(action, groupJid, false, "no participants given");
        return;
    }
    PendingGroupOp op;
    op.kind = action;
    op.jid = groupJid;
    op.detail = QString::number(jids.size());
    sendGroupOp(op, wa::messaging::buildGroupParticipantsRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString(),
                        parsed, jids));
    DLOG(this, QString("group %1: %2 %3 participant(s)")
                   .arg(groupJid)
                   .arg(action)
                   .arg(jids.size()));
}

void WhatsAppDirect::requestGroupInfo(const QString &groupJid) {
    if (groupJid.isEmpty()) return;
    PendingGroupOp op;
    op.kind = "info";
    op.jid = groupJid;
    sendGroupOp(op, wa::messaging::buildGroupMetadataRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString()));
}

void WhatsAppDirect::requestGroupInvite(const QString &groupJid) {
    PendingGroupOp op;
    op.kind = "invite";
    op.jid = groupJid;
    sendGroupOp(op, wa::messaging::buildGroupInviteRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString(), false));
}

void WhatsAppDirect::revokeGroupInvite(const QString &groupJid) {
    PendingGroupOp op;
    op.kind = "revoke";
    op.jid = groupJid;
    sendGroupOp(op, wa::messaging::buildGroupInviteRequest(
                        wa::messaging::generateMessageId(), groupJid.toStdString(), true));
}

// Turns a group IQ reply into signals. Errors are reported per operation, and
// cached metadata is dropped when the reply makes it stale.
void WhatsAppDirect::handleGroupOpResult(const QString &iqId, const wa::BinaryNode &node) {
    const PendingGroupOp op = m_pendingGroupOps.take(iqId);
    const bool isError = node.attrOr("type") == "error";
    const std::string kind = op.kind.toStdString();
    std::string error;

    if (op.kind == "create") {
        wa::messaging::GroupInfo info;
        if (isError || !wa::messaging::parseGroupCreated(node, info, error)) {
            if (error.empty()) error = "group create failed";
            DLOG(this, "group create failed: " + QString::fromStdString(error));
            emit groupActionResult(op.kind, QString(), false,
                                   QString::fromStdString(error));
            return;
        }
        const QString jid = QString::fromStdString(info.jid);
        if (info.subject.empty()) info.subject = op.detail.toStdString();
        m_groupCache.insert(jid, info);
        Chat chat = chatModelFor(info.jid);
        chat.name = QString::fromStdString(info.subject);
        chat.isGroup = true;
        emit chatUpdated(chat);
        emit groupCreated(jid, chat.name);
        emit groupActionResult(op.kind, jid, true, QString());
        DLOG(this, "group created: " + jid + " (" + chat.name + ")");
        return;
    }

    if (op.kind == "join") {
        std::string joined;
        if (isError || !wa::messaging::parseGroupAcceptInvite(node, joined, error)) {
            if (error.empty()) error = "join failed";
            emit groupActionResult(op.kind, QString(), false,
                                   QString::fromStdString(error));
            return;
        }
        const QString jid = QString::fromStdString(joined);
        // New member: fetch the metadata so the chat list shows the subject.
        m_groupCache.remove(jid);
        Chat chat = chatModelFor(joined);
        chat.isGroup = true;
        emit groupCreated(jid, chat.name);
        emit groupActionResult(op.kind, jid, true, QString());
        PendingGroupOp refresh;
        refresh.kind = "metadata";
        refresh.jid = jid;
        sendGroupOp(refresh, wa::messaging::buildGroupMetadataRequest(
                                 wa::messaging::generateMessageId(), joined));
        DLOG(this, "joined group " + jid);
        return;
    }

    if (op.kind == "info") {
        wa::messaging::GroupInfo meta;
        if (isError || !wa::messaging::parseGroupMetadata(node, meta, error)) {
            if (error.empty()) error = "group metadata failed";
            emit groupActionResult("info", op.jid, false,
                                   QString::fromStdString(error));
            return;
        }
        m_groupCache.insert(op.jid, meta);
        GroupInfo group;
        group.jid = QString::fromStdString(meta.jid);
        group.subject = QString::fromStdString(meta.subject);
        group.description = QString::fromStdString(meta.description);
        group.owner = QString::fromStdString(meta.owner);
        const std::string me = m_meJid.toStdString();
        for (const wa::messaging::GroupMember &member : meta.members) {
            GroupParticipant participant;
            participant.jid = QString::fromStdString(member.jid);
            const std::string user =
                wa::jidDecode(member.jid).user;
            participant.phoneNumber = QString::fromStdString(user);
            if (member.superAdmin) participant.admin = "superadmin";
            else if (member.admin) participant.admin = "admin";
            participant.isAdmin = member.admin;
            participant.isSuperAdmin = member.superAdmin;
            if (wa::areJidsSameUser(member.jid, me)) participant.name = "you";
            group.participants.append(participant);
        }
        group.size = int(meta.members.size());
        emit groupReady(group);
        emit groupActionResult("info", op.jid, true, QString());
        return;
    }

    if (op.kind == "metadata") {
        wa::messaging::GroupInfo info;
        if (isError || !wa::messaging::parseGroupMetadata(node, info, error)) {
            DLOG(this, "group metadata refresh failed for " + op.jid +
                           ": " + QString::fromStdString(error));
            return;
        }
        if (info.subject.empty()) info.subject = op.jid.toStdString();
        m_groupCache.insert(op.jid, info);
        Chat chat = chatModelFor(info.jid);
        chat.name = QString::fromStdString(info.subject);
        chat.isGroup = true;
        emit chatUpdated(chat);
        return;
    }

    if (op.kind == "description-meta") {
        wa::messaging::GroupInfo info;
        if (isError || !wa::messaging::parseGroupMetadata(node, info, error)) {
            if (error.empty()) error = "group metadata failed";
            emit groupActionResult("description", op.jid, false,
                                   QString::fromStdString(error));
            return;
        }
        m_groupCache.insert(op.jid, info);
        PendingGroupOp edit;
        edit.kind = "description";
        edit.jid = op.jid;
        edit.detail = op.detail;
        sendGroupOp(edit,
                    wa::messaging::buildGroupDescriptionRequest(
                        wa::messaging::generateMessageId(), op.jid.toStdString(),
                        op.detail.toStdString(), info.descId,
                        op.detail.isEmpty() ? std::string()
                                            : wa::messaging::generateMessageId()));
        return;
    }

    if (op.kind == "invite" || op.kind == "revoke") {
        std::string code;
        if (isError || !wa::messaging::parseGroupInviteCode(node, code, error)) {
            if (error.empty()) error = "invite failed";
            emit groupActionResult(op.kind, op.jid, false,
                                   QString::fromStdString(error));
            return;
        }
        emit groupInviteCode(op.jid, QString::fromStdString(code));
        emit groupActionResult(op.kind, op.jid, true, QString());
        DLOG(this, "group invite code for " + op.jid + ": " +
                       QString::fromStdString(code));
        return;
    }

    const bool isParticipants = op.kind == "add" || op.kind == "remove" ||
                                op.kind == "promote" || op.kind == "demote";
    if (isParticipants) {
        wa::messaging::GroupAction action = wa::messaging::GroupAction::Add;
        wa::messaging::parseGroupAction(kind, action);
        std::vector<wa::messaging::ParticipantChange> changes;
        if (isError ||
            !wa::messaging::parseGroupParticipantsResult(node, action, changes, error)) {
            if (error.empty()) error = "participant update failed";
            emit groupActionResult(op.kind, op.jid, false,
                                   QString::fromStdString(error));
            return;
        }
        QStringList failed;
        for (const wa::messaging::ParticipantChange &change : changes) {
            if (change.ok()) continue;
            failed << QString::fromStdString(change.jid + " (" + change.status + ")");
        }
        m_groupCache.remove(op.jid);   // membership changed
        if (failed.isEmpty()) {
            emit groupActionResult(op.kind, op.jid, true, QString());
            DLOG(this, QString("group %1: %2 ok for %3 participant(s)")
                           .arg(op.jid)
                           .arg(op.kind)
                           .arg(changes.size()));
        } else {
            const QString reason = "failed for " + failed.join(", ");
            emit groupActionResult(op.kind, op.jid, false, reason);
            DLOG(this, "group " + op.jid + ": " + op.kind + " " + reason);
        }
        return;
    }

    // leave / subject / description: an iq result is all the server sends.
    if (isError) {
        const wa::BinaryNode *err = node.child("error");
        std::string text = err ? err->attrOr("text", err->attrOr("code", "?")) : "?";
        emit groupActionResult(op.kind, op.jid, false, QString::fromStdString(text));
        DLOG(this, "group " + op.kind + " failed: " + QString::fromStdString(text));
        return;
    }
    if (op.kind == "leave") m_groupCache.remove(op.jid);
    if (op.kind == "subject" || op.kind == "description") {
        if (m_groupCache.contains(op.jid)) {
            wa::messaging::GroupInfo info = m_groupCache.value(op.jid);
            if (op.kind == "subject") info.subject = op.detail.toStdString();
            else if (!op.detail.isEmpty())
                info.descId = op.detail.toStdString();
            m_groupCache.insert(op.jid, info);
            Chat chat = chatModelFor(info.jid);
            chat.name = QString::fromStdString(info.subject);
            chat.isGroup = true;
            emit chatUpdated(chat);
        } else {
            m_groupCache.remove(op.jid);
        }
    }
    emit groupActionResult(op.kind, op.jid, true, QString());
    DLOG(this, "group " + op.kind + " done for " + op.jid);
}

// ─── Key bundles, pre-key upload ───────────────────────────────────────────

void WhatsAppDirect::ensurePreKeys(int count) {
    while (m_preKeys.size() < count) {
        const wa::signal::KeyPair kp = wa::signal::generateKeyPair();
        QJsonObject o;
        o["id"] = int(m_nextPreKeyId++);
        // Store the raw 32-byte halves; the Signal layer adds the 0x05 prefix.
        o["priv"] = b64(toQ(kp.priv));
        o["pub"] = b64(QByteArray(reinterpret_cast<const char *>(kp.pub.data()) + 1, 32));
        m_preKeys.append(o);
    }
}

void WhatsAppDirect::uploadPreKeys() {
    if (m_meJid.isEmpty()) return;
    ensurePreKeys(30);
    if (m_preKeys.isEmpty()) return;

    BinaryNode iq("iq");
    iq.setAttr("id", (m_meJid + "-" + QString::number(m_nextPreKeyId)).toStdString());
    iq.setAttr("type", "set");
    iq.setAttr("xmlns", "encrypt");
    iq.setAttr("to", "s.whatsapp.net");

    // <registration> is big-endian uint32, <type> is the 0x05 key bundle type.
    Bytes reg(4);
    reg[0] = uint8_t(m_registrationId >> 24);
    reg[1] = uint8_t(m_registrationId >> 16);
    reg[2] = uint8_t(m_registrationId >> 8);
    reg[3] = uint8_t(m_registrationId);
    iq.addChild(BinaryNode("registration").setBytes(reg));
    iq.addChild(BinaryNode("type").setBytes(Bytes{0x05}));
    iq.addChild(BinaryNode("identity").setBytes(toStd(m_identityPub)));

    BinaryNode list("list");
    quint32 uploaded = m_lastUploadedPreKeyId;
    for (const QJsonValue &v : m_preKeys) {
        const QJsonObject o = v.toObject();
        const quint32 id = quint32(o.value("id").toInt());
        if (id <= m_lastUploadedPreKeyId) continue;   // already on the server
        BinaryNode key("key");
        key.addChild(BinaryNode("id").setBytes(Bytes{uint8_t(id >> 16), uint8_t(id >> 8),
                                                     uint8_t(id)}));
        key.addChild(BinaryNode("value").setBytes(toStd(fromB64(o.value("pub").toString()))));
        list.addChild(key);
        uploaded = qMax(uploaded, id);
    }
    if (!list.hasContent()) return;   // nothing new to publish
    iq.addChild(list);

    quint32 skId = m_signedPreKeyId;
    BinaryNode skey("skey");
    skey.addChild(BinaryNode("id").setBytes(Bytes{uint8_t(skId >> 16), uint8_t(skId >> 8),
                                                  uint8_t(skId)}));
    skey.addChild(BinaryNode("value").setBytes(toStd(m_signedPreKeyPub)));
    skey.addChild(BinaryNode("signature").setBytes(toStd(m_signedPreKeySig)));
    iq.addChild(skey);

    sendNode(iq);
    m_preKeysUploaded = true;
    m_lastUploadedPreKeyId = uploaded;
    saveCredentials();
    DLOG(this, QString("uploaded one-time pre-keys up to id %1 (%2 held)")
                   .arg(uploaded)
                   .arg(m_preKeys.size()));
}

void WhatsAppDirect::requestGroupMetadata(const PendingSend &pending) {
    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    m_pendingGroupMeta.insert(iqId, pending);
    DLOG(this, "fetching group metadata for " + pending.jid);
    sendNode(wa::messaging::buildGroupMetadataRequest(iqId.toStdString(),
                                                      pending.jid.toStdString()));
}

void WhatsAppDirect::fetchGroupBundles(const PendingSend &pending,
                                       const wa::messaging::GroupInfo &info) {
    // One <iq xmlns=encrypt> for every participant plus ourselves (our other
    // devices need the distribution message too, and our own session must be
    // ready for the echo of our own group message).
    std::vector<std::string> users;
    users.push_back(m_meJid.toStdString());
    for (const auto &p : info.participants) {
        if (wa::messaging::sameUser(p, m_meJid.toStdString())) continue;
        users.push_back(p);
    }

    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    m_pendingSends.insert(iqId, pending);
    DLOG(this, QString("fetching device bundles for %1 participant(s) of %2")
                   .arg(int(users.size()))
                   .arg(pending.jid));
    sendNode(wa::messaging::buildKeyRequest(iqId.toStdString(), users));
}

void WhatsAppDirect::sendGroupWithBundles(const PendingSend &pending,
                                          const wa::messaging::GroupInfo &info,
                                          std::vector<wa::messaging::DeviceBundle> devices) {
    Q_UNUSED(info)
    // Drop this very device; the server echoes our own message back.
    std::vector<wa::messaging::DeviceBundle> filtered;
    for (const auto &d : devices) {
        if (QString::fromStdString(d.jid) == m_meJid) continue;
        filtered.push_back(d);
    }
    if (filtered.empty()) {
        emit sendResult(pending.msgId, false,
                        "no reachable devices in " + pending.jid);
        return;
    }

    std::set<std::string> already;
    const QSet<QString> memory = m_senderKeyMemory.value(pending.jid);
    for (const QString &d : memory) already.insert(d.toStdString());

    wa::messaging::GroupSendPlan plan;
    std::string error;
    const bool built =
        pending.isMedia
            ? wa::messaging::buildGroupMessageStanza(*m_store, *m_senderKeys,
                                                    m_meJid.toStdString(),
                                                    pending.jid.toStdString(),
                                                    pending.payload,
                                                    pending.msgId.toStdString(), filtered,
                                                    already, plan, error)
            : wa::messaging::buildGroupTextStanza(*m_store, *m_senderKeys,
                                                 m_meJid.toStdString(),
                                                 pending.jid.toStdString(),
                                                 pending.text.toStdString(),
                                                 pending.msgId.toStdString(), filtered,
                                                 already, plan, error);
    if (!built) {
        emit sendResult(pending.msgId, false, QString::fromStdString(error));
        DLOG(this, "group encrypt failed for " + pending.jid + ": " +
                       QString::fromStdString(error));
        return;
    }

    sendNode(plan.stanza);
    QSet<QString> updated = memory;
    for (const auto &jid : plan.distributedTo)
        updated.insert(QString::fromStdString(jid));
    m_senderKeyMemory.insert(pending.jid, updated);

    DLOG(this, QString("sent group %1 to %2 (%3 device bundle(s), %4 distribution message(s))")
                   .arg(pending.msgId)
                   .arg(pending.jid)
                   .arg(int(filtered.size()))
                   .arg(int(plan.distributedTo.size())));
    emit sendResult(pending.msgId, true, QString());

    Message echo;
    echo.id = pending.msgId;
    echo.chatJid = pending.jid;
    echo.fromMe = true;
    echo.text = pending.text;
    echo.timestamp = QDateTime::currentMSecsSinceEpoch();
    echo.status = "sent";
    emit messageAdded(echo);
}

void WhatsAppDirect::finishSend(const QString &iqId, const wa::BinaryNode &iqNode) {
    if (!m_pendingSends.contains(iqId)) return;
    const PendingSend pending = m_pendingSends.take(iqId);

    std::vector<wa::messaging::DeviceBundle> devices;
    std::string error;
    const QString userJid =
        QString::fromStdString(wa::jidNormalizedUser(pending.jid.toStdString()));

    if (!wa::messaging::parseKeyResponse(iqNode, devices, error)) {
        emit sendResult(pending.msgId, false, QString::fromStdString(error));
        DLOG(this, "key fetch failed for " + pending.jid + ": " +
                       QString::fromStdString(error));
        return;
    } else if (pending.isGroup) {
        // Group bundles are cached per participant jid; the message is
        // encrypted once, so the participant list (not the cache) drives it.
        wa::messaging::GroupInfo info = m_groupCache.value(pending.jid);
        sendGroupWithBundles(pending, info, devices);
        return;
    } else {
        // Never encrypt to this very device; the server echoes to others.
        std::vector<wa::messaging::DeviceBundle> filtered;
        for (const auto &d : devices) {
            if (QString::fromStdString(d.jid) == m_meJid) continue;
            filtered.push_back(d);
        }
        devices.swap(filtered);
        CachedBundles cached;
        cached.fetchedAt = QDateTime::currentMSecsSinceEpoch();
        cached.devices = devices;
        m_bundleCache.insert(userJid, cached);
    }

    sendWithBundles(pending, devices);
}

void WhatsAppDirect::sendWithBundles(const PendingSend &pending,
                                     std::vector<wa::messaging::DeviceBundle> devices) {
    std::string error;
    if (devices.empty()) {
        emit sendResult(pending.msgId, false, "no reachable devices for " + pending.jid);
        return;
    }

    wa::messaging::SendPlan plan;
    const bool built =
        pending.isMedia
            ? wa::messaging::buildMessageStanza(*m_store, m_meJid.toStdString(),
                                                pending.jid.toStdString(), pending.payload,
                                                pending.msgId.toStdString(), devices, plan,
                                                error)
            : wa::messaging::buildTextStanza(*m_store, m_meJid.toStdString(),
                                             pending.jid.toStdString(),
                                             pending.text.toStdString(),
                                             pending.msgId.toStdString(), devices, plan,
                                             error);
    if (!built) {
        emit sendResult(pending.msgId, false, QString::fromStdString(error));
        DLOG(this, "encrypt failed for " + pending.jid + ": " +
                       QString::fromStdString(error));
        return;
    }

    sendNode(plan.stanza);
    DLOG(this, QString("sent %1 to %2 device(s)%3")
                   .arg(pending.msgId)
                   .arg(plan.deviceCount)
                   .arg(plan.usedPreKey ? " (pre-key message)" : ""));
    emit sendResult(pending.msgId, true, QString());

    Message echo;
    echo.id = pending.msgId;
    echo.chatJid = pending.jid;
    echo.fromMe = true;
    echo.senderJid = m_meJid;
    echo.text = pending.text;
    echo.timestamp = qt_now_secs();
    echo.status = QStringLiteral("sent");
    emit messageAdded(echo);
}
