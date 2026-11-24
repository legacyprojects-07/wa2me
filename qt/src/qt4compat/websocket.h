// Minimal WebSocket client for Qt 4.7.
//
// QWebSocket only appeared in Qt 5.3 and the Symbian toolchain stops at Qt
// 4.7.4, so direct mode speaks RFC 6455 over QSslSocket itself: the HTTP
// Upgrade handshake, client-side masking, fragmentation, ping/pong and the
// close handshake. Only what wss://web.whatsapp.com/ws/chat needs is
// implemented (binary messages, no extensions, no sub-protocols).
#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSslError>
#include <QString>
#include <QUrl>

class QSslSocket;
class QTimer;

class QtWsSocket : public QObject {
    Q_OBJECT
public:
    explicit QtWsSocket(QObject *parent = 0);
    ~QtWsSocket();

    // Starts a TLS connection + the WebSocket upgrade. `origin` is sent as the
    // Origin header (WhatsApp rejects requests without it).
    void open(const QUrl &url, const QByteArray &origin, const QByteArray &userAgent);
    void close();
    void abort();

    void sendBinaryMessage(const QByteArray &payload);
    bool isConnected() const { return m_ready; }
    QString errorString() const { return m_lastError; }

signals:
    void connected();
    void disconnected();
    void binaryMessageReceived(const QByteArray &payload);
    void error(const QString &reason);

private slots:
    void onEncrypted();
    void onReadyRead();
    void onSocketClosed();
    void onSocketError(QAbstractSocket::SocketError code);
    void onSslErrors(const QList<QSslError> &errors);
    void onHandshakeTimeout();

private:
    void sendHttpHandshake();
    bool processHttpResponse();
    void processFrames();
    void sendFrame(quint8 opcode, const QByteArray &payload);
    void fail(const QString &reason);

    QSslSocket *m_socket;
    QTimer *m_timeout;
    QUrl m_url;
    QByteArray m_origin;
    QByteArray m_userAgent;
    QByteArray m_buffer;
    QByteArray m_handshakeKey;
    QByteArray m_fragment;      // reassembled fragmented message
    quint8 m_fragmentOpcode;
    bool m_httpDone;
    bool m_ready;
    bool m_closeSent;
    QString m_lastError;
};
