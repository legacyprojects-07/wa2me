#include "websocket.h"

#include <QHostAddress>
#include <QSslSocket>
#include <QTimer>

#include "../direct/sha256.h"

namespace {

const char *const kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Opcodes (RFC 6455 §5.2).
enum Opcode {
    OpContinuation = 0x0,
    OpText = 0x1,
    OpBinary = 0x2,
    OpClose = 0x8,
    OpPing = 0x9,
    OpPong = 0xA,
};

QByteArray base64(const QByteArray &data) { return data.toBase64(); }

} // namespace

QtWsSocket::QtWsSocket(QObject *parent)
    : QObject(parent),
      m_socket(new QSslSocket(this)),
      m_timeout(new QTimer(this)),
      m_fragmentOpcode(OpContinuation),
      m_httpDone(false),
      m_ready(false),
      m_closeSent(false) {
    connect(m_socket, SIGNAL(encrypted()), this, SLOT(onEncrypted()));
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onSocketClosed()));
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this,
            SLOT(onSocketError(QAbstractSocket::SocketError)));
    connect(m_socket, SIGNAL(sslErrors(QList<QSslError>)), this,
            SLOT(onSslErrors(QList<QSslError>)));

    m_timeout->setSingleShot(true);
    m_timeout->setInterval(20000);
    connect(m_timeout, SIGNAL(timeout()), this, SLOT(onHandshakeTimeout()));
}

QtWsSocket::~QtWsSocket() {
    // The socket is a child; stop it from emitting into a half-destroyed object.
    disconnect(m_socket, 0, this, 0);
}

void QtWsSocket::open(const QUrl &url, const QByteArray &origin,
                      const QByteArray &userAgent) {
    m_url = url;
    m_origin = origin;
    m_userAgent = userAgent;
    m_buffer.clear();
    m_fragment.clear();
    m_fragmentOpcode = OpContinuation;
    m_httpDone = false;
    m_ready = false;
    m_closeSent = false;
    m_lastError.clear();

    const QString host = url.host();
    const quint16 port = quint16(url.port(443));

    // A fresh 16-byte nonce for Sec-WebSocket-Key.
    const wa::crypto::Bytes random = wa::crypto::randomBytes(16);
    m_handshakeKey = base64(QByteArray(reinterpret_cast<const char *>(random.data()),
                                       int(random.size())));

    // Symbian's root store is older than WhatsApp's certificate chain; allow an
    // opt-in override instead of silently weakening TLS.
    if (!qgetenv("OPENKNOT_INSECURE_TLS").isEmpty())
        m_socket->setPeerVerifyMode(QSslSocket::VerifyNone);

    m_socket->connectToHostEncrypted(host, port);
    m_timeout->start();
}

void QtWsSocket::close() {
    if (!m_ready) {
        abort();
        return;
    }
    if (!m_closeSent) {
        sendFrame(OpClose, QByteArray());
        m_closeSent = true;
    }
    m_socket->flush();
    m_socket->disconnectFromHost();
    m_ready = false;
}

void QtWsSocket::abort() {
    m_timeout->stop();
    m_ready = false;
    m_socket->abort();
}

void QtWsSocket::fail(const QString &reason) {
    m_lastError = reason;
    m_timeout->stop();
    m_ready = false;
    emit error(reason);
    m_socket->abort();
}

void QtWsSocket::onHandshakeTimeout() {
    if (m_ready) return;
    fail("websocket handshake timed out");
}

void QtWsSocket::onSslErrors(const QList<QSslError> &errors) {
    if (!qgetenv("OPENKNOT_INSECURE_TLS").isEmpty()) {
        m_socket->ignoreSslErrors();
        return;
    }
    QString reason = "TLS error";
    if (!errors.isEmpty()) reason += ": " + errors.first().errorString();
    reason += " (set OPENKNOT_INSECURE_TLS=1 to accept the certificate anyway)";
    fail(reason);
}

void QtWsSocket::onSocketError(QAbstractSocket::SocketError) {
    fail(m_socket->errorString());
}

void QtWsSocket::onSocketClosed() {
    m_timeout->stop();
    const bool wasReady = m_ready;
    m_ready = false;
    if (wasReady) emit disconnected();
}

void QtWsSocket::onEncrypted() { sendHttpHandshake(); }

void QtWsSocket::sendHttpHandshake() {
    const QString host = m_url.host();
    const QString path = m_url.path().isEmpty() ? QString("/") : m_url.path();
    QByteArray request;
    request += "GET " + path.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + host.toUtf8() + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + m_handshakeKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    if (!m_origin.isEmpty()) request += "Origin: " + m_origin + "\r\n";
    if (!m_userAgent.isEmpty()) request += "User-Agent: " + m_userAgent + "\r\n";
    request += "\r\n";
    m_socket->write(request);
    m_socket->flush();
}

void QtWsSocket::onReadyRead() {
    m_buffer += m_socket->readAll();
    if (!m_httpDone) {
        if (!processHttpResponse()) return;
    }
    processFrames();
}

bool QtWsSocket::processHttpResponse() {
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > 32 * 1024) fail("oversized websocket handshake response");
        return false;
    }
    const QByteArray head = m_buffer.left(end);
    m_buffer.remove(0, end + 4);

    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        fail("empty websocket handshake response");
        return false;
    }
    const QByteArray status = lines.first().trimmed();
    if (!status.contains("101")) {
        fail("websocket upgrade refused: " + QString::fromLatin1(status));
        return false;
    }

    // Verify Sec-WebSocket-Accept = base64(SHA1(key + GUID)) when present.
    QByteArray accept;
    for (int i = 1; i < lines.size(); i++) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        if (name == "sec-websocket-accept")
            accept = line.mid(colon + 1).trimmed();
    }
    if (!accept.isEmpty()) {
        const QByteArray signed_ = m_handshakeKey + kGuid;
        const wa::crypto::Bytes digest = wa::crypto::sha1(
            wa::crypto::Bytes(signed_.constData(),
                              signed_.constData() + signed_.size()));
        const QByteArray expected = base64(
            QByteArray(reinterpret_cast<const char *>(digest.data()), int(digest.size())));
        if (expected != accept) {
            fail("websocket accept hash mismatch");
            return false;
        }
    }

    m_httpDone = true;
    m_ready = true;
    m_timeout->stop();
    emit connected();
    return true;
}

void QtWsSocket::sendFrame(quint8 opcode, const QByteArray &payload) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return;

    QByteArray frame;
    frame += char(0x80 | (opcode & 0x0f));   // FIN + opcode

    const int size = payload.size();
    if (size < 126) {
        frame += char(0x80 | size);          // masked, 7-bit length
    } else if (size <= 0xffff) {
        frame += char(0x80 | 126);
        frame += char((size >> 8) & 0xff);
        frame += char(size & 0xff);
    } else {
        frame += char(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame += char((quint64(size) >> shift) & 0xff);
    }

    // Client frames must be masked with a fresh 32-bit key (RFC 6455 §5.3).
    const wa::crypto::Bytes maskBytes = wa::crypto::randomBytes(4);
    const QByteArray mask(reinterpret_cast<const char *>(maskBytes.data()), 4);
    frame += mask;

    QByteArray masked(payload);
    for (int i = 0; i < masked.size(); i++) masked[i] = char(masked.at(i) ^ mask.at(i % 4));
    frame += masked;

    m_socket->write(frame);
}

void QtWsSocket::sendBinaryMessage(const QByteArray &payload) {
    if (!m_ready) return;
    sendFrame(OpBinary, payload);
    m_socket->flush();
}

void QtWsSocket::processFrames() {
    while (true) {
        if (m_buffer.size() < 2) return;
        const quint8 b0 = quint8(m_buffer.at(0));
        const quint8 b1 = quint8(m_buffer.at(1));
        const bool fin = (b0 & 0x80) != 0;
        const quint8 opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        quint64 length = quint64(b1 & 0x7f);
        int offset = 2;

        if (length == 126) {
            if (m_buffer.size() < 4) return;
            length = (quint64(quint8(m_buffer.at(2))) << 8) | quint64(quint8(m_buffer.at(3)));
            offset = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10) return;
            length = 0;
            for (int i = 0; i < 8; i++)
                length = (length << 8) | quint64(quint8(m_buffer.at(2 + i)));
            offset = 10;
        }

        QByteArray mask;
        if (masked) {
            if (m_buffer.size() < offset + 4) return;
            mask = m_buffer.mid(offset, 4);
            offset += 4;
        }
        if (quint64(m_buffer.size()) < quint64(offset) + length) return;   // partial frame

        QByteArray payload = m_buffer.mid(offset, int(length));
        m_buffer.remove(0, offset + int(length));
        if (masked) {
            for (int i = 0; i < payload.size(); i++)
                payload[i] = char(payload.at(i) ^ mask.at(i % 4));
        }

        switch (opcode) {
        case OpBinary:
        case OpText:
            if (!fin) {
                m_fragment = payload;
                m_fragmentOpcode = opcode;
            } else {
                emit binaryMessageReceived(payload);
            }
            break;
        case OpContinuation:
            m_fragment += payload;
            if (fin) {
                emit binaryMessageReceived(m_fragment);
                m_fragment.clear();
                m_fragmentOpcode = OpContinuation;
            }
            break;
        case OpPing:
            sendFrame(OpPong, payload);
            m_socket->flush();
            break;
        case OpPong:
            break;
        case OpClose:
            if (!m_closeSent) {
                sendFrame(OpClose, QByteArray());
                m_closeSent = true;
            }
            m_ready = false;
            m_socket->disconnectFromHost();
            emit disconnected();
            return;
        default:
            break;
        }
    }
}
