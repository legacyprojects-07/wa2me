#pragma once
// WhatsAppDirect — DIRECT MODE: Qt app talks to wss://web.whatsapp.com itself.
//
// This replaces the Node backend for the *transport* half of Baileys:
//   RFC 6455 socket + Noise (curves/AES-GCM/SHA-256, all dependency-free here)
//   + BinaryNode codec + handshake protobuf + QR pairing + reconnect/ping.
//
// It deliberately mirrors wa.js: the same high-level signals ApiClient emits
// (chats/messages/send) exist here, so MainWindow can switch modes without
// caring which one is live.
//
// Status (see QT_PORT.md §3):
//   WORKING   : websocket lifecycle, Noise handshake, framing, QR pairing
//               payloads, stream error handling, keepalive, stanza routing,
//               1:1 text send/receive (Signal), group send/receive
//               (sender keys, skmsg), media upload/download (CDN),
//               HistorySync download+decode
//   TODO      : app state (mute/pin/archive), presence privacy.

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QString>
#include "qt4compat/json.h"
#include "qt4compat/qt4compat.h"
#include "qt4compat/websocket.h"
#include <QVariant>
#include <QHash>
#include <QSet>

#include <memory>
#include <vector>

#include "binary_node.h"
#include "models.h"
#include "noise.h"
#include "media_upload.h"
#include "sender_key.h"
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
    // as a <message> stanza. Group jids take the sender-key (skmsg) path:
    // group metadata is fetched first, the body encrypted once, and the
    // sender-key distribution message fanned out to devices that lack it.
    // Failures are reported through sendResult().
    void sendText(const QString &jid, const QString &text);

    // Media send: encrypt the file with a fresh mediaKey, POST it to the CDN
    // (media_conn -> upload URL), then send an image/video/audio/document
    // message carrying url + directPath + mediaKey + hashes. Groups use the
    // sender-key path like text does.
    void sendMedia(const QString &jid, const QString &filePath,
                   const QString &caption = QString());

    // Media download: fetch the message's CDN url, verify + decrypt it and
    // write `savePath`. Reports through mediaSaved()/mediaFailed().
    bool downloadMedia(const QString &chatJid, const QString &msgId,
                       const QString &savePath);

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
    void mediaSaved(const QString &chatJid, const QString &msgId, const QString &path);
    void mediaFailed(const QString &chatJid, const QString &msgId, const QString &error);
    void mediaProgress(const QString &msgId, const QString &stage);
    void logLine(const QString &line);

    // Queued sends/uploads (declared before the methods that take them).
    struct PendingSend {
        QString jid;
        QString text;
        QString msgId;
        bool isGroup = false;
        // Media sends carry a fully assembled message (built after the upload
        // completes) instead of `text`.
        bool isMedia = false;
        wa::waproto::Message payload;
    };
    // An upload in flight: the send waits until the CDN hands back a URL.
    struct PendingUpload {
        PendingSend send;
        wa::media::Type type = wa::media::Type::Document;
        wa::media::UploadedMedia uploaded;
        wa::media::MediaMeta meta;
    };

private slots:
    void onConnected();
    void onDisconnected();
    void onBinaryMessage(const QByteArray &data);
    void onSocketError(const QString &reason);
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
    class SenderKeysStore;                  // wa::signal::SenderKeyStore (groups)

    void ensurePreKeys(int count);          // generate one-time pre-keys
    void uploadPreKeys();                   // <iq type=set xmlns=encrypt>
    void finishSend(const QString &iqId, const wa::BinaryNode &iqNode);
    void sendWithBundles(const PendingSend &pending,
                         std::vector<wa::messaging::DeviceBundle> devices);
    // Media plumbing (CDN).
    void enqueueSend(const PendingSend &pending);
    void startMediaUpload(const PendingSend &pending);
    void finishMediaUpload(const QString &uploadId, const QByteArray &body,
                           const QString &errorText);
    void registerMedia(const QString &msgId, wa::media::Type type,
                       const wa::waproto::MediaInfo &info);

    void requestGroupMetadata(const PendingSend &pending);
    void fetchGroupBundles(const PendingSend &pending,
                           const wa::messaging::GroupInfo &info);
    void sendGroupWithBundles(const PendingSend &pending,
                              const wa::messaging::GroupInfo &info,
                              std::vector<wa::messaging::DeviceBundle> devices);
    void sendReceipt(const wa::BinaryNode &node);
    void downloadHistory(const wa::waproto::HistorySyncNotification &note);
    Chat chatModelFor(const std::string &jid) const;
    Message messageModelFor(const wa::messaging::IncomingMessage &in) const;

    struct CachedBundles {
        qint64 fetchedAt = 0;
        std::vector<wa::messaging::DeviceBundle> devices;
    };

    std::unique_ptr<CredsStore> m_store;
    std::unique_ptr<SenderKeysStore> m_senderKeys;
    QNetworkAccessManager *m_net = nullptr;
    QHash<QString, PendingSend> m_pendingSends;   // iq id -> queued message
    QHash<QString, CachedBundles> m_bundleCache;  // user jid -> device bundles
    wa::media::MediaConn m_mediaConn;             // cached <media_conn>
    QHash<QString, PendingUpload> m_pendingUploads;   // upload id -> in-flight send
    QHash<QString, PendingSend> m_waitingForMediaConn; // iq id -> queued send
    // msgId -> (media kind, CDN metadata) so a later tap can download it.
    QHash<QString, QPair<int, wa::waproto::MediaInfo> > m_mediaIndex;
    QJsonArray m_preKeys;      // [{id, priv, pub}] one-time pre-keys
    QJsonObject m_sessions;    // address -> hex(signal::serializeRecord)
    QJsonObject m_senderKeyJson;   // "<group>::<user>" -> serialized sender key
    QHash<QString, wa::messaging::GroupInfo> m_groupCache;
    QHash<QString, PendingSend> m_pendingGroupMeta;   // iq id -> queued group send
    // group jid -> device jids that already hold our sender key (Baileys'
    // sender-key-memory, which keeps the distribution fan-out small).
    QHash<QString, QSet<QString> > m_senderKeyMemory;
    QHash<QString, QString> m_senderKeyGroupNames;  // device jid -> group, for memory
    quint32 m_nextPreKeyId = 1;
    quint32 m_lastUploadedPreKeyId = 0;
    wa::history::SyncData m_history;
    bool m_preKeysUploaded = false;

    QtWsSocket m_socket;   // RFC 6455 over QSslSocket (QWebSocket is Qt 5.3+)
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
