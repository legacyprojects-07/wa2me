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
//               (sender keys, skmsg), group admin (create/join/leave/
//               participants/subject/description/invite), media
//               upload/download (CDN), HistorySync download+decode
//   TODO      : app state (mute/pin/archive), presence privacy.

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include "qt4compat/json.h"
#include "qt4compat/qt4compat.h"
#include "qt4compat/websocket.h"
#include <QVariant>
#include <QHash>
#include <QSet>

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
    ~WhatsAppDirect();

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
    // `voiceNote` sends audio as a push-to-talk message (ptt=true, duration
    // read from the Ogg granule position) — the "voice note" media template.
    void sendMedia(const QString &jid, const QString &filePath,
                   const QString &caption = QString(), bool voiceNote = false);

    // Media download: fetch the message's CDN url, verify + decrypt it and
    // write `savePath`. Reports through mediaSaved()/mediaFailed().
    bool downloadMedia(const QString &chatJid, const QString &msgId,
                       const QString &savePath);

    // ── Group administration ────────────────────────────────────────────
    // Same operations the proxy exposes as /groups/…, sent as the <iq
    // xmlns=w:g2> stanzas Baileys sends (lib/Socket/groups.js). Replies arrive
    // through groupActionResult(); create/join/invite have their own signals.
    void createGroup(const QString &subject, const QStringList &participants);
    void joinGroup(const QString &inviteCode);
    void leaveGroup(const QString &groupJid);
    void setGroupSubject(const QString &groupJid, const QString &subject);
    void setGroupDescription(const QString &groupJid, const QString &description);
    // `action` is "add" | "remove" | "promote" | "demote".
    void updateGroupParticipants(const QString &groupJid,
                                 const QStringList &participants,
                                 const QString &action);
    void requestGroupInvite(const QString &groupJid);   // fetch the code
    void revokeGroupInvite(const QString &groupJid);    // invalidate the code
    // Group metadata for display; answers with groupReady() (same model the
    // REST client emits).
    void requestGroupInfo(const QString &groupJid);


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
    // Group administration: `action` is the operation ("create", "add",
    // "subject", …). Failures carry the server's error text.
    void groupActionResult(const QString &action, const QString &jid, bool ok,
                           const QString &error);
    void groupCreated(const QString &groupJid, const QString &subject);
    void groupJoined(const QString &groupJid);
    void groupInviteCode(const QString &groupJid, const QString &code);
    void groupReady(const GroupInfo &group);   // metadata for the info dialog
    // Server-side group change (someone renamed the group, changed membership…).
    // `kind` is the notification child (subject/create/add/remove/promote/
    // demote/modify/description/leave) and `detail` its human summary.
    void groupNotification(const QString &groupJid, const QString &kind,
                           const QString &detail);
    void mediaSaved(const QString &chatJid, const QString &msgId, const QString &path);
    void mediaFailed(const QString &chatJid, const QString &msgId, const QString &error);
    void mediaProgress(const QString &msgId, const QString &stage);
    void logLine(const QString &line);

private slots:
    void onConnected();
    void onDisconnected();
    void onBinaryMessage(const QByteArray &data);
    void onSocketError(const QString &reason);
    void sendKeepAlive();

private:
    // Queued sends/uploads (declared before the methods that take them).
    struct PendingSend {
        QString jid;
        QString text;
        QString msgId;
        bool isGroup;
        // Media sends carry a fully assembled message (built after the upload
        // completes) instead of `text`.
        bool isMedia;
        wa::waproto::Message payload;

        PendingSend() : isGroup(false), isMedia(false) {}
    };
    // An upload in flight: the send waits until the CDN hands back a URL.
    struct PendingUpload {
        PendingSend send;
        wa::media::Type type;
        wa::media::UploadedMedia uploaded;
        wa::media::MediaMeta meta;

        PendingUpload() : type(wa::media::Type::Document) {}
    };
    // A group administration IQ waiting for its reply.
    struct PendingGroupOp {
        QString kind;     // create|join|leave|subject|description|add|remove|…
        QString jid;      // group jid (empty for create/join)
        QString detail;   // subject/description text, participant count
    };


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
    void handleGroupNotification(const wa::BinaryNode &node);

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

    void sendGroupOp(const PendingGroupOp &op, const wa::BinaryNode &node);
    void handleGroupOpResult(const QString &iqId, const wa::BinaryNode &node);
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
        qint64 fetchedAt;
        std::vector<wa::messaging::DeviceBundle> devices;
    CachedBundles() : fetchedAt(0) {}
    };

    CredsStore *m_store;
    SenderKeysStore *m_senderKeys;
    QNetworkAccessManager *m_net;
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
    QHash<QString, PendingGroupOp> m_pendingGroupOps; // iq id -> group admin op
    // group jid -> device jids that already hold our sender key (Baileys'
    // sender-key-memory, which keeps the distribution fan-out small).
    QHash<QString, QSet<QString> > m_senderKeyMemory;
    QHash<QString, QString> m_senderKeyGroupNames;  // device jid -> group, for memory
    quint32 m_nextPreKeyId;
    quint32 m_lastUploadedPreKeyId;
    wa::history::SyncData m_history;
    bool m_preKeysUploaded;

    QtWsSocket m_socket;   // RFC 6455 over QSslSocket (QWebSocket is Qt 5.3+)
    wa::noise::NoiseHandler *m_noise;
    QTimer m_keepAlive;
    QTimer m_reconnectTimer;

    QString m_credsPath;
    QByteArray m_noisePriv, m_noisePub;   // long-lived creds noiseKey
    QByteArray m_ephPriv, m_ephPub;       // per-connection ephemeral
    QByteArray m_keyEnc;                  // encrypted static from ClientFinish
    QByteArray m_identityPriv, m_identityPub;
    QByteArray m_signedPreKeyPriv, m_signedPreKeyPub, m_signedPreKeySig;
    quint32 m_signedPreKeyId;
    quint32 m_registrationId;
    QString m_meJid;
    QByteArray m_advSecretKey;

    QString m_browserOs;
    QString m_browserName;
    bool m_syncFullHistory;
    bool m_markOnlineOnConnect;
    bool m_hasMe;
    bool m_connected;
    bool m_handshakeDone;
    bool m_manualDisconnect;
    int m_reconnectAttempts;

    // App version reported to the server (Baileys ships a versioned number;
    // update via setAppVersion when WhatsApp revs the web client).
    quint32 m_appVersionPrimary;
    quint32 m_appVersionSecondary;
    quint32 m_appVersionTertiary;
};
