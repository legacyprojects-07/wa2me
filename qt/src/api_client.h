#pragma once
// ApiClient — PROXY MODE: full OpenKnot REST client (see index.js).
// Uses QNetworkAccessManager + QJsonDocument. No WebSocket, no crypto libs,
// so this builds and runs on Symbian Qt 4.7 (with a JSON-parser shim) as
// well as Qt5/Qt6 desktop. This is the drop-in Qt replacement for the
// J2ME client: same HTTP API, same JSON shapes (serializers.js).

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QFile>
#include <QList>

#include <functional>

#include "models.h"

class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(QObject *parent = nullptr);

    void setBaseUrl(const QString &url);   // e.g. http://192.168.1.10:3000
    void setApiKey(const QString &key);    // optional X-API-Key

    // ── System ──────────────────────────────────────────────────────────
    void getHealth();
    void getRoot();
    void getVersion();
    void getStats();
    void getQr();            // JSON {status, qr(dataURL)}
    void getQrImage();       // PNG bytes -> qrImageReady(QImage)
    void resetAuth();

    // ── Chats ───────────────────────────────────────────────────────────
    void getChats(int limit = 200, int offset = 0, bool all = false);
    void getChat(const QString &jid);
    void markChatRead(const QString &jid);
    void markChatUnread(const QString &jid, int count = 1);
    void setChatTyping(const QString &jid, const QString &status); // composing|paused|recording
    void subscribePresence(const QString &jid);
    void setChatArchived(const QString &jid, bool archived);
    void setChatPinned(const QString &jid, bool pinned);
    void setChatMuted(const QString &jid, bool muted, int durationSeconds = 8*60*60);
    void clearChat(const QString &jid, bool remote = true);
    void deleteChat(const QString &jid, bool remote = true);

    // ── Messages ────────────────────────────────────────────────────────
    void getMessages(const QString &jid, int limit = 100,
                     qint64 before = -1, qint64 after = -1, bool markRead = true);
    void getMessage(const QString &jid, const QString &msgId);
    void getMedia(const QString &jid, const QString &msgId,
                  bool thumb = false, bool download = false);
    void deleteMessage(const QString &jid, const QString &msgId,
                       const QString &scope = "everyone"); // everyone|me
    void forwardMessage(const QString &fromJid, const QString &msgId, const QString &toJid);
    void reactMessage(const QString &jid, const QString &msgId, const QString &emoji);
    void editMessage(const QString &jid, const QString &msgId, const QString &text);

    // ── Search / sync ───────────────────────────────────────────────────
    void searchMessages(const QString &q, const QString &jid = QString(),
                        int limit = 100, int offset = 0);
    void sync(qint64 sinceMs = 0, int limit = 200); // delta polling

    // ── Send ────────────────────────────────────────────────────────────
    void sendText(const QString &jid, const QString &text,
                  const QString &quotedId = QString());
    void sendMedia(const QString &jid, const QString &filePath,
                   const QString &mediaType = QString(), // auto-detect if empty
                   const QString &caption = QString(),
                   const QString &quotedId = QString(),
                   bool ptt = false);
    void sendContact(const QString &jid, const QString &name, const QString &phone,
                     const QString &quotedId = QString());
    void sendLocation(const QString &jid, double lat, double lng,
                      const QString &name = QString(), const QString &address = QString(),
                      const QString &quotedId = QString());

    // ── Profile / contacts / presence ─────────────────────────────────
    void getMe();
    void updateMyStatus(const QString &status);
    void updateMyName(const QString &name);
    void setMyPresence(const QString &status); // available|unavailable
    void getUser(const QString &jid);
    void checkOnWhatsApp(const QString &phone);
    void checkOnWhatsAppBatch(const QStringList &phones);
    void getContacts(const QString &q = QString());
    void getBlocked();
    void setBlocked(const QString &jid, bool blocked);
    void getPresence(const QString &jid = QString()); // empty = all
    void getProfilePic(const QString &jid, bool preview = false);

    // ── Groups ──────────────────────────────────────────────────────────
    void getGroups(int limit = 200, int offset = 0);
    void createGroup(const QString &subject, const QStringList &participants);
    void joinGroup(const QString &codeOrLink);
    void getGroup(const QString &jid);
    void updateGroupParticipants(const QString &jid, const QStringList &participants,
                                 const QString &action); // add|remove|promote|demote
    void leaveGroup(const QString &jid);
    void getGroupInvite(const QString &jid);
    void revokeGroupInvite(const QString &jid);
    void updateGroupSubject(const QString &jid, const QString &subject);
    void updateGroupDescription(const QString &jid, const QString &description);
    void setGroupSetting(const QString &jid, const QString &setting);

signals:
    void healthReady(const QJsonObject &obj);
    void rootReady(const QJsonObject &obj);
    void versionReady(const QJsonObject &obj);
    void statsReady(const QJsonObject &obj);
    void qrReady(const QString &status, const QString &qrDataUrl);
    void qrImageReady(const QByteArray &pngBytes);
    void resetDone(const QJsonObject &obj);
    void chatsReady(const QList<Chat> &chats);
    void chatReady(const QJsonObject &obj);
    void chatActionDone(const QString &action, const QJsonObject &obj);
    void messagesReady(const QString &jid, const QList<Message> &messages);
    void messageReady(const Message &msg);
    void mediaReady(const QString &jid, const QString &msgId,
                    const QByteArray &bytes, const QString &mime);
    void messageActionDone(const QString &action, const QJsonObject &obj);
    void searchReady(const QList<Message> &messages);
    void syncReady(qint64 nextSince, bool hasMore,
                   const QList<Chat> &chats, const QList<Message> &messages);
    void sendDone(const QJsonObject &obj);
    void meReady(const QJsonObject &obj);
    void profileActionDone(const QString &action, const QJsonObject &obj);
    void userReady(const QJsonObject &obj);
    void checkReady(const QJsonObject &obj);
    void checkBatchReady(const QJsonArray &arr);
    void contactsReady(const QList<Contact> &contacts);
    void blockedReady(const QJsonArray &arr);
    void presenceReady(const QJsonObject &obj);
    void profilePicReady(const QString &jid, const QByteArray &bytes);
    void groupsReady(const QList<Chat> &groups);
    void groupReady(const GroupInfo &group);
    void groupActionDone(const QString &action, const QJsonObject &obj);
    void errorOccurred(const QString &message, const QString &code, int httpStatus);

private:
    QNetworkAccessManager *m_nam;
    QString m_baseUrl;
    QString m_apiKey;

    QUrl url(const QString &path) const;
    QNetworkReply *get(const QString &path, const QMap<QString, QString> &query = {});
    QNetworkReply *postJson(const QString &path, const QJsonObject &body);
    QNetworkReply *postForm(const QString &path, const QMap<QString, QString> &fields);
    QNetworkReply *postMultipart(const QString &path,
                                 const QMap<QString, QString> &fields,
                                 const QList<QFile*> &files,
                                 const QString &fileField = "file");
    void setAuthHeader(QNetworkRequest &req) const;
    // Runs `ok` on a successful JSON reply, or emits errorOccurred otherwise.
    void handleJson(QNetworkReply *reply, std::function<void(const QJsonDocument &)> ok);
    static QString enc(const QString &s); // percent-encode path segment
    static Chat parseChat(const QJsonObject &o);
    static Message parseMessage(const QJsonObject &o);
    static Contact parseContact(const QJsonObject &o);
    static GroupInfo parseGroup(const QJsonObject &o);
    static QString guessMediaType(const QString &filePath);
    static QString guessMime(const QString &filePath);
};
