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
//               payloads, stream error handling, keepalive, stanza routing
//   TODO      : Signal (X3DH/Double Ratchet) message encryption, WAProto
//               Message/HistorySync decoding, media CDN crypto, app state.
//               Those need libsignal-protocol-c + generated WAProto; the
//               hooks are marked `TODO(signal)` / `TODO(proto)` below.

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QByteArray>
#include <QString>
#include <QJsonObject>
#include <QVariantMap>

#include <memory>

#include "binary_node.h"
#include "noise.h"

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

    // Minimal send API. Text send requires Signal encryption (TODO(signal));
    // messages are queued and the encryption hook is where libsignal plugs in.
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
    void incomingMessage(const QJsonObject &message);      // TODO(proto) populates
    void messageStatus(const QString &chatJid, const QString &msgId,
                       const QString &status);
    void presenceUpdate(const QString &jid, const QString &presence);
    void historySyncReceived(int progress, int chats, int messages); // TODO(proto)

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

    // Message handling hooks that need the full WAProto + Signal stack.
    void handleEncryptedMessage(const wa::BinaryNode &node);   // TODO(signal)
    void handleReceipt(const wa::BinaryNode &node);
    void handlePresence(const wa::BinaryNode &node);

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
