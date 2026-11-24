#include "whatsapp_direct.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QDebug>

#include "aes_gcm.h"
#include "proto_min.h"
#include "sha256.h"
#include "x25519.h"

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
    // TODO(signal): the pre-key signature is an XEdDSA signature over the
    // 33-byte prefixed public key. XEdDSA needs libsignal-protocol-c; until
    // then we still register, but the server rejects pre-key upload.
    m_signedPreKeySig = QByteArray(64, '\0');
    m_signedPreKeyId = 1;

    m_registrationId = QRandomGenerator::global()->bounded(1u, 0x3ffffeu);
    m_advSecretKey = randomBytes32();
    m_meJid.clear();
    m_hasMe = false;
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
}

void WhatsAppDirect::replyToIq(const BinaryNode &iq, const QString &resultTag) {
    BinaryNode reply(resultTag.toStdString());
    reply.setAttr("id", iq.attrOr("id"));
    reply.setAttr("type", "result");
    reply.setAttr("to", iq.attrOr("from", "s.whatsapp.net"));
    sendNode(reply);
}

// ─── Message hooks (need Signal + WAProto) ─────────────────────────────────

void WhatsAppDirect::handleEncryptedMessage(const BinaryNode &node) {
    // TODO(signal): decrypt <enc> payload with the Signal session for
    // (from, participant) using libsignal-protocol-c, then decode
    // proto.Message with the generated WAProto and emit incomingMessage().
    // Reference: Baileys Socket/messages-recv.js + Signal/libsignal.js.
    const QString from = QString::fromStdString(node.attrOr("from"));
    const QString id = QString::fromStdString(node.attrOr("id"));
    DLOG(this, QString("encrypted message %1 from %2 - Signal decryption not "
                       "implemented yet").arg(id, from));
    emit messageStatus(from, id, "received-undecryptable");
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
    // TODO(signal): build proto.Message {conversation:text}, encrypt per device
    // (encryptMessage in Baileys), then send <message> with the <enc> children.
    // Until that exists we refuse loudly instead of silently dropping messages.
    Q_UNUSED(text)
    const QString id = QString("3EB0%1").arg(QDateTime::currentMSecsSinceEpoch(), 0, 16);
    emit sendResult(id, false,
                    "direct-mode sending needs Signal sessions (see QT_PORT.md §6)");
    DLOG(this, "sendText blocked: Signal encryption not implemented for " + jid);
}
