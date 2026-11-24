#pragma once
// WhatsAppDirect — DIRECT MODE: Qt app talks to wss://web.whatsapp.com itself.
//
// This replaces the Node backend for the *transport* half of Baileys:
//   QWebSocket + Noise (curves/AES-GCM/SHA-256, all dependency-free here)
//   + BinaryNode codec + handshake protobuf + QR pairing + reconnect/ping.
//
// It deliberately mirrors wa.js: the same high-level signals ApiClient emits
// (chats/messages/send) exist here, so MainWindow can switch modes without
// caring which one is live.
//
// Status (see QT_PORT.md §3):
//   WORKING   : websocket lifecycle, Noise handshake, framing, QR pairing
//               payloads, stream error handling, keepalive, stanza routing,
//               1:1 text send/receive (Signal), HistorySync download+decode
//   TODO      : group sender-key messages (skmsg), media upload/download,
//               app state (mute/pin/archive), presence privacy.

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QByteArray>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#include <QHash>

#include <memory>
#include <vector>

#include "binary_node.h"
#include "models.h"
#include "noise.h"
#include "signal.h"
#include "wa_history.h"
#include "wa_message.h"

class QNetworkAccessManager;

class WhatsAppDirect : public QObject {
    Q_OBJECT
public:
    explicit WhatsAppDirect(QObject *parent = nullptr);
    ~WhatsAppDirect() override;

    // Where credentials are persisted (creds.json equivalent).
    void setCredentialsPath(const QString &path);
    QString credentialsPath() const { return m_credsPath; }

    // Options mirroring DEFAULT_CONNECTION_CONFIG in Baileys/Defaults.
    void setSyncFullHistory(bool on) { m_syncFullHistory = on; }
    void setMarkOnlineOnConnect(bool on) { m_markOnlineOnConnect = on; }
    void setBrowser(const QString &os, const QString &browser);

    bool isConnected() const { return m_connected; }
    bool isRegistered() const { return m_hasMe; }
    QString myJid() const { return m_meJid; }

public slots:
    void connectToWhatsApp();
    void disconnectFromWhatsApp();
    void logout();                       // clears credentials, then disconnects
    void requestPairingQr();              // ask the server for a fresh QR

    // Text send. Device bundles are fetched with <iq xmlns=encrypt> the first
    // time, then cached; the message is Signal-encrypted per device and sent
    // as a <message> stanza. Failures are reported through sendResult().
    void sendText(const QString &jid, const QString &text);

signals:
    // Connection lifecycle
    void connectionUpdate(const QString &state, const QString &reason);
    void qrCode(const QString &qr);          // raw QR string from pair-device
    void paired(const QString &jid);         // "me" assigned after pairing
    void loggedOut();
    void streamError(const QString &code, const QString &text);
    void transportReady();                   // Noise handshake finished

    // Stanzas (decoded BinaryNodes, exposed as maps for the UI/debug view)
    void nodeReceived(const QString &tag, const QVariantMap &attrs);
    void incomingMessage(const QJsonObject &message);      // raw dump for debugging
    void messageStatus(const QString &chatJid, const QString &msgId,
                       const QString &status);
    void presenceUpdate(const QString &jid, const QString &presence);

    // Decoded traffic for MainWindow (same models the REST client emits).
    void chatUpdated(const Chat &chat);
    void messageAdded(const Message &message);
    void historySyncReceived(int progress, int chats, int messages);

    void sendResult(const QString &id, bool ok, const QString &error);
    void logLine(const QString &line);

private slots:
    void onConnected();
    void onDisconnected();
    void onBinaryMessage(const QByteArray &data);
    void onSocketError(QAbstractSocket::SocketError error);
    void sendKeepAlive();

private:
    // Handshake
    void sendClientHello();
    bool handleServerHello(const QByteArray &frame);
    bool sendClientFinish();
    void startKeepAlive();

    // Stanza plumbing
    void sendNode(const wa::BinaryNode &node);
    void handleNode(const wa::BinaryNode &node);
    void handlePairDevice(const wa::BinaryNode &node);
    void handleSuccess(const wa::BinaryNode &node);
    void replyToIq(const wa::BinaryNode &iq, const QString &resultTag);

    // Credentials (creds.json equivalent, JSON on disk)
    bool loadCredentials();
    bool saveCredentials();
    void generateNewCredentials();
    QByteArray randomBytes32();

    // Message handling (Signal + WAProto, see wa_message.h / wa_history.h).
    void handleEncryptedMessage(const wa::BinaryNode &node);
    void handleReceipt(const wa::BinaryNode &node);
    void handlePresence(const wa::BinaryNode &node);

    // ── Signal / message plumbing ────────────────────────────────────────
    class CredsStore;                       // wa::signal::Store over creds.json

    void ensurePreKeys(int count);          // generate one-time pre-keys
    void uploadPreKeys();                   // <iq type=set xmlns=encrypt>
    void finishSend(const QString &iqId, const wa::BinaryNode &iqNode);
    void sendWithBundles(const PendingSend &pending,
                         std::vector<wa::messaging::DeviceBundle> devices);
    void sendReceipt(const wa::BinaryNode &node);
    void downloadHistory(const wa::waproto::HistorySyncNotification &note);
    Chat chatModelFor(const std::string &jid) const;
    Message messageModelFor(const wa::messaging::IncomingMessage &in) const;

    struct PendingSend {
        QString jid;
        QString text;
        QString msgId;
    };
    struct CachedBundles {
        qint64 fetchedAt = 0;
        std::vector<wa::messaging::DeviceBundle> devices;
    };

    std::unique_ptr<CredsStore> m_store;
    QNetworkAccessManager *m_net = nullptr;
    QHash<QString, PendingSend> m_pendingSends;   // iq id -> queued message
    QHash<QString, CachedBundles> m_bundleCache;  // user jid -> device bundles
    QJsonArray m_preKeys;      // [{id, priv, pub}] one-time pre-keys
    QJsonObject m_sessions;    // address -> hex(signal::serializeRecord)
    quint32 m_nextPreKeyId = 1;
    quint32 m_lastUploadedPreKeyId = 0;
    wa::history::SyncData m_history;
    bool m_preKeysUploaded = false;

    QWebSocket m_socket;
    std::unique_ptr<wa::noise::NoiseHandler> m_noise;
    QTimer m_keepAlive;
    QTimer m_reconnectTimer;

    QString m_credsPath;
    QByteArray m_noisePriv, m_noisePub;   // long-lived creds noiseKey
    QByteArray m_ephPriv, m_ephPub;       // per-connection ephemeral
    QByteArray m_keyEnc;                  // encrypted static from ClientFinish
    QByteArray m_identityPriv, m_identityPub;
    QByteArray m_signedPreKeyPriv, m_signedPreKeyPub, m_signedPreKeySig;
    quint32 m_signedPreKeyId = 1;
    quint32 m_registrationId = 0;
    QString m_meJid;
    QByteArray m_advSecretKey;

    QString m_browserOs = "Ubuntu";
    QString m_browserName = "Chrome";
    bool m_syncFullHistory = true;
    bool m_markOnlineOnConnect = true;
    bool m_hasMe = false;
    bool m_connected = false;
    bool m_handshakeDone = false;
    bool m_manualDisconnect = false;
    int m_reconnectAttempts = 0;

    // App version reported to the server (Baileys ships a versioned number;
    // update via setAppVersion when WhatsApp revs the web client).
    quint32 m_appVersionPrimary = 2;
    quint32 m_appVersionSecondary = 3000;
    quint32 m_appVersionTertiary = 1035939187;
};
