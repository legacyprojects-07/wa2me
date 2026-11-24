#include "whatsapp_direct.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
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
    connect(&m_keepAlive, &QTimer::timeout, this, &WhatsAppDirect::sendKeepAlive);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_manualDisconnect) return;
        // Baileys: reconnect after 5s unless the session was logged out.
        m_reconnectAttempts++;
        emit connectionUpdate("connecting", QString("reconnect #%1").arg(m_reconnectAttempts));
        connectToWhatsApp();
    });

    connect(&m_socket, &QWebSocket::connected, this, &WhatsAppDirect::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &WhatsAppDirect::onDisconnected);
    connect(&m_socket, &QWebSocket::binaryMessageReceived,
            this, &WhatsAppDirect::onBinaryMessage);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(&m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError e) { onSocketError(e); });
#else
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError e) { onSocketError(e); });
#endif

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

    m_registrationId = QRandomGenerator::global()->bounded(1u, 0x3ffffeu);
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

    QNetworkRequest req(QUrl(QString::fromLatin1(kWebSocketUrl)));
    // WhatsApp requires the browser Origin and a desktop User-Agent.
    req.setRawHeader("Origin", kOrigin);
    req.setRawHeader("User-Agent",
                     QString("Mozilla/5.0 (%1) AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/120.0.0.0 Safari/537.36")
                         .arg(m_browserOs)
                         .toUtf8());

    emit connectionUpdate("connecting", "opening websocket");
    DLOG(this, "connecting to " + QString::fromLatin1(kWebSocketUrl));
    m_socket.open(req);
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

void WhatsAppDirect::onSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error)
    emit connectionUpdate("error", m_socket.errorString());
    DLOG(this, "socket error: " + m_socket.errorString());
}

// ─── Handshake ─────────────────────────────────────────────────────────────

void WhatsAppDirect::sendClientHello() {
    // Baileys generates a fresh ephemeral pair per socket and builds the Noise
    // handler on it; the long-lived creds noiseKey is only used in ClientFinish.
    const wa::crypto::CurveKeyPair eph = wa::crypto::generateCurveKeyPair();
    m_ephPriv = toQ(eph.priv);
    m_ephPub = toQ(eph.pub);
    m_noise = std::make_unique<wa::noise::NoiseHandler>(eph.priv, eph.pub);

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
    payload.userAgent.appVersion = {m_appVersionPrimary, m_appVersionSecondary, m_appVersionTertiary};
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
        if (m_pendingSends.contains(iqId) && (iqType == "result" || iqType == "error")) {
            finishSend(iqId, node);
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
            if (info.subject.isEmpty())
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, carried]() {
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
    const QString msgId =
        QString::fromStdString(wa::messaging::generateMessageId());

    if (wa::isJidGroup(jid.toStdString())) {
        PendingSend pending{jid, text, msgId, true};
        const auto cached = m_groupCache.constFind(jid);
        if (cached != m_groupCache.constEnd() && !cached->participants.empty()) {
            fetchGroupBundles(pending, *cached);
        } else {
            requestGroupMetadata(pending);
        }
        return;
    }

    // Device bundles are cached for a few minutes (Baileys' USER_DEVICES TTL).
    const QString userJid = QString::fromStdString(
        wa::jidNormalizedUser(jid.toStdString()));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto cached = m_bundleCache.find(userJid);
    if (cached != m_bundleCache.end() &&
        now - cached->fetchedAt < 5 * 60 * 1000 &&
        !cached->devices.empty()) {
        sendWithBundles(PendingSend{jid, text, msgId}, cached->devices);
        return;
    }

    const QString iqId = QString::fromStdString(wa::messaging::generateMessageId());
    m_pendingSends.insert(iqId, PendingSend{jid, text, msgId});

    const std::vector<std::string> request{
        jid.toStdString(), m_meJid.toStdString()};
    const wa::BinaryNode iq =
        wa::messaging::buildKeyRequest(iqId.toStdString(), request);
    DLOG(this, "fetching device bundles for " + jid + "/" + m_meJid);
    sendNode(iq);
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
    iq.setAttr("id", m_meJid + "-" + QString::number(m_nextPreKeyId).toStdString());
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
        if (wa::sameUser(p, m_meJid.toStdString())) continue;
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
    if (!wa::messaging::buildGroupTextStanza(*m_store, *m_senderKeys, m_meJid.toStdString(),
                                            pending.jid.toStdString(),
                                            pending.text.toStdString(),
                                            pending.msgId.toStdString(), filtered, already,
                                            plan, error)) {
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
        m_bundleCache.insert(userJid, CachedBundles{QDateTime::currentMSecsSinceEpoch(),
                                                    devices});
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
    if (!wa::messaging::buildTextStanza(*m_store, m_meJid.toStdString(),
                                        pending.jid.toStdString(),
                                        pending.text.toStdString(),
                                        pending.msgId.toStdString(), devices, plan,
                                        error)) {
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
    echo.timestamp = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    echo.status = QStringLiteral("sent");
    emit messageAdded(echo);
}
