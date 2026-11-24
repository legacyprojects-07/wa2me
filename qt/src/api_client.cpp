#include "api_client.h"
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QFileInfo>
#include <QUrlQuery>
#include <QDateTime>

// ─── Helpers (mirrors helpers.js) ────────────────────────────────────────────

// Same behaviour as helpers.js normalizeJid: bare phone numbers get the
// @s.whatsapp.net suffix; anything carrying "@" passes through untouched.
static QString normalizeJid(const QString &in) {
    const QString jid = in.trimmed();
    if (jid.contains('@')) return jid;

    QString cleaned;
    cleaned.reserve(jid.size());
    for (const QChar ch : jid) {
        if (ch.isSpace() || ch == '(' || ch == ')' || ch == '+' || ch == '.' || ch == '-') continue;
        cleaned += ch;
    }
    bool digitsOnly = !cleaned.isEmpty();
    for (const QChar ch : cleaned) {
        if (!ch.isDigit()) { digitsOnly = false; break; }
    }
    if (digitsOnly && cleaned.size() >= 5) return cleaned + "@s.whatsapp.net";
    return jid;
}

ApiClient::ApiClient(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void ApiClient::setBaseUrl(const QString &url) {
    m_baseUrl = url.trimmed();
    while (m_baseUrl.endsWith('/')) m_baseUrl.chop(1);
}
void ApiClient::setApiKey(const QString &key) { m_apiKey = key.trimmed(); }

QUrl ApiClient::url(const QString &path) const {
    return QUrl(m_baseUrl + path);
}

QString ApiClient::enc(const QString &s) {
    return QString::fromUtf8(QUrl::toPercentEncoding(s));
}

void ApiClient::setAuthHeader(QNetworkRequest &req) const {
    if (!m_apiKey.isEmpty()) req.setRawHeader("X-API-Key", m_apiKey.toUtf8());
}

QNetworkReply *ApiClient::get(const QString &path, const QMap<QString, QString> &query) {
    QUrl u = url(path);
    if (!query.isEmpty()) {
        QUrlQuery q;
        for (auto it = query.constBegin(); it != query.constEnd(); ++it) q.addQueryItem(it.key(), it.value());
        u.setQuery(q);
    }
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, "OpenKnot-Qt/1.0");
    setAuthHeader(req);
    return m_nam->get(req);
}

QNetworkReply *ApiClient::postJson(const QString &path, const QJsonObject &body) {
    QNetworkRequest req(url(path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "OpenKnot-Qt/1.0");
    setAuthHeader(req);
    return m_nam->post(req, QJsonDocument(body).toJson());
}

QNetworkReply *ApiClient::postForm(const QString &path, const QMap<QString, QString> &fields) {
    QUrlQuery q;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) q.addQueryItem(it.key(), it.value());
    QNetworkRequest req(url(path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setHeader(QNetworkRequest::UserAgentHeader, "OpenKnot-Qt/1.0");
    setAuthHeader(req);
    return m_nam->post(req, q.toString(QUrl::FullyEncoded).toUtf8());
}

QNetworkReply *ApiClient::postMultipart(const QString &path,
                                        const QMap<QString, QString> &fields,
                                        const QList<QFile*> &files,
                                        const QString &fileField) {
    QHttpMultiPart *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"").arg(it.key()));
        part.setBody(it.value().toUtf8());
        mp->append(part);
    }
    for (QFile *f : files) {
        QHttpPart part;
        QString fname = QFileInfo(f->fileName()).fileName();
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"; filename=\"%2\"").arg(fileField, fname));
        part.setHeader(QNetworkRequest::ContentTypeHeader, guessMime(f->fileName()));
        part.setBodyDevice(f);
        mp->append(part);
    }
    QNetworkRequest req(url(path));
    req.setHeader(QNetworkRequest::UserAgentHeader, "OpenKnot-Qt/1.0");
    setAuthHeader(req);
    QNetworkReply *reply = m_nam->post(req, mp);
    mp->setParent(reply); // ownership: reply deletes multipart
    return reply;
}

void ApiClient::handleJson(QNetworkReply *reply,
                           std::function<void(const QJsonDocument &)> ok) {
    connect(reply, &QNetworkReply::finished, this, [this, reply, ok]() {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const QJsonDocument d = QJsonDocument::fromJson(body);
            QString msg = reply->errorString();
            QString code = "NETWORK_ERROR";
            if (d.isObject()) {
                if (d.object().contains("error")) msg = d.object().value("error").toString();
                if (d.object().contains("code")) code = d.object().value("code").toString();
            }
            emit errorOccurred(msg, code, status);
            return;
        }

        const QJsonDocument d = QJsonDocument::fromJson(body);
        if (d.isNull()) {
            emit errorOccurred("Invalid JSON response", "BAD_JSON", status);
            return;
        }
        ok(d);
    });
}

// ─── Parsing (mirrors serializers.js) ────────────────────────────────────────

Chat ApiClient::parseChat(const QJsonObject &o) {
    Chat c;
    c.jid = o.value("jid").toString();
    c.id = o.value("id").toString(c.jid);
    c.name = o.value("name").toString();
    c.isGroup = o.value("isGroup").toBool();
    c.unreadCount = o.value("unreadCount").toInt();
    c.lastMessageTs = o.value("lastMessageTs").toVariant().toLongLong();
    c.lastMessage = o.value("lastMessage").toString();
    c.archived = o.value("archived").toBool();
    c.pinned = o.value("pinned").toBool();
    c.muted = o.value("muted").toBool();
    c.muteUntil = o.value("muteUntil").toVariant().toLongLong();
    return c;
}

Message ApiClient::parseMessage(const QJsonObject &o) {
    Message m;
    m.id = o.value("id").toString();
    m.chatJid = o.value("chatJid").toString();
    m.fromMe = o.value("fromMe").toBool();
    m.senderJid = o.value("senderJid").toString();
    m.senderName = o.value("senderName").toString();
    m.timestamp = o.value("timestamp").toVariant().toLongLong();
    m.text = o.value("text").toString();
    m.mediaType = o.value("mediaType").toString();
    m.hasMedia = o.value("hasMedia").toBool();
    m.mediaUrl = o.value("mediaUrl").toString();
    m.mediaMime = o.value("mediaMime").toString();
    m.fileName = o.value("fileName").toString();
    m.mediaSeconds = o.value("mediaSeconds").toInt();
    m.isPtt = o.value("isPtt").toBool();
    m.status = o.value("status").toString("received");
    m.deleted = o.value("deleted").toBool();
    m.deletedAt = o.value("deletedAt").toVariant().toLongLong();
    m.edited = o.value("edited").toBool();
    m.quotedId = o.value("quotedId").toString();
    if (o.value("quoted").isObject()) {
        QJsonObject q = o.value("quoted").toObject();
        m.quoted.id = q.value("id").toString();
        m.quoted.fromMe = q.value("fromMe").toBool();
        m.quoted.senderName = q.value("senderName").toString();
        m.quoted.mediaType = q.value("mediaType").toString();
        m.quoted.deleted = q.value("deleted").toBool();
        m.quoted.text = q.value("text").toString();
    }
    for (const QJsonValue &rv : o.value("reactions").toArray()) {
        QJsonObject r = rv.toObject();
        m.reactions.append({r.value("senderJid").toString(),
                            r.value("emoji").toString(),
                            r.value("fromMe").toBool()});
    }
    return m;
}

Contact ApiClient::parseContact(const QJsonObject &o) {
    Contact c;
    c.jid = o.value("jid").toString();
    c.name = o.value("name").toString();
    c.notify = o.value("notify").toString();
    c.phoneNumber = o.value("phoneNumber").toString();
    c.isSaved = o.value("isSaved").toBool();
    return c;
}

GroupInfo ApiClient::parseGroup(const QJsonObject &o) {
    GroupInfo g;
    g.jid = o.value("jid").toString();
    g.subject = o.value("subject").toString();
    g.description = o.value("description").toString();
    g.owner = o.value("owner").toString();
    g.createdAt = o.value("createdAt").toVariant().toLongLong();
    g.isAnnounce = o.value("isAnnounce").toBool();
    g.isLocked = o.value("isLocked").toBool();
    g.isCommunity = o.value("isCommunity").toBool();
    g.size = o.value("size").toInt();
    for (const QJsonValue &pv : o.value("participants").toArray()) {
        QJsonObject p = pv.toObject();
        GroupParticipant gp;
        gp.jid = p.value("jid").toString();
        gp.phoneNumber = p.value("phoneNumber").toString();
        gp.name = p.value("name").toString();
        gp.admin = p.value("admin").toString();
        gp.isAdmin = p.value("isAdmin").toBool();
        gp.isSuperAdmin = p.value("isSuperAdmin").toBool();
        g.participants.append(gp);
    }
    return g;
}

QString ApiClient::guessMediaType(const QString &filePath) {
    QString ext = QFileInfo(filePath).suffix().toLowerCase();
    if (ext=="jpg"||ext=="jpeg"||ext=="png"||ext=="gif"||ext=="bmp"||ext=="heic"||ext=="webp") return "image";
    if (ext=="mp4"||ext=="3gp"||ext=="3gpp"||ext=="mkv"||ext=="webm"||ext=="m4v") return "video";
    if (ext=="ogg"||ext=="oga"||ext=="opus"||ext=="mp3"||ext=="m4a"||ext=="aac"||ext=="amr"||ext=="wav") return "audio";
    return "document";
}

QString ApiClient::guessMime(const QString &filePath) {
    QString ext = QFileInfo(filePath).suffix().toLowerCase();
    static const QMap<QString,QString> m = {
        {"jpg","image/jpeg"},{"jpeg","image/jpeg"},{"png","image/png"},{"gif","image/gif"},
        {"webp","image/webp"},{"bmp","image/bmp"},{"heic","image/heic"},
        {"mp4","video/mp4"},{"3gp","video/3gpp"},{"3gpp","video/3gpp"},{"mkv","video/x-matroska"},
        {"webm","video/webm"},{"ogg","audio/ogg"},{"opus","audio/ogg"},{"mp3","audio/mpeg"},
        {"m4a","audio/mp4"},{"aac","audio/aac"},{"amr","audio/amr"},{"wav","audio/wav"},
        {"pdf","application/pdf"},{"zip","application/zip"},{"txt","text/plain"},
        {"vcf","text/vcard"},{"apk","application/vnd.android.package-archive"},
    };
    return m.value(ext, "application/octet-stream");
}

// ─── System ──────────────────────────────────────────────────────────────────

void ApiClient::getHealth() {
    handleJson(get("/health"), [this](const QJsonDocument &d){ emit healthReady(d.object()); });
}
void ApiClient::getRoot() {
    handleJson(get("/"), [this](const QJsonDocument &d){ emit rootReady(d.object()); });
}
void ApiClient::getVersion() {
    handleJson(get("/version"), [this](const QJsonDocument &d){ emit versionReady(d.object()); });
}
void ApiClient::getStats() {
    handleJson(get("/stats"), [this](const QJsonDocument &d){ emit statsReady(d.object()); });
}
void ApiClient::getQr() {
    handleJson(get("/qr"), [this](const QJsonDocument &d){
        emit qrReady(d.object().value("status").toString(),
                     d.object().value("qr").toString());
    });
}
void ApiClient::getQrImage() {
    QNetworkReply *r = get("/qr-image", {{"size","256"}});
    QObject::connect(r, &QNetworkReply::finished, this, [this, r](){
        QByteArray b = r->readAll();
        int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r->deleteLater();
        if (code == 200 && !b.isEmpty()) emit qrImageReady(b);
        else emit errorOccurred("QR image unavailable", "NO_QR", code);
    });
}
void ApiClient::resetAuth() {
    handleJson(postJson("/reset", {}), [this](const QJsonDocument &d){ emit resetDone(d.object()); });
}

// ─── Chats ───────────────────────────────────────────────────────────────────

void ApiClient::getChats(int limit, int offset, bool all) {
    QMap<QString,QString> q = {{"limit", QString::number(limit)},
                               {"offset", QString::number(offset)}};
    if (all) q["all"] = "1";
    handleJson(get("/chats", q), [this](const QJsonDocument &d){
        QList<Chat> out;
        for (const QJsonValue &v : d.array()) out.append(parseChat(v.toObject()));
        emit chatsReady(out);
    });
}
void ApiClient::getChat(const QString &jid) {
    handleJson(get("/chats/" + enc(normalizeJid(jid))),
           [this](const QJsonDocument &d){ emit chatReady(d.object()); });
}
void ApiClient::markChatRead(const QString &jid) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/read", {}),
           [this](const QJsonDocument &d){ emit chatActionDone("read", d.object()); });
}
void ApiClient::markChatUnread(const QString &jid, int count) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/unread", {{"count", count}}),
           [this](const QJsonDocument &d){ emit chatActionDone("unread", d.object()); });
}
void ApiClient::setChatTyping(const QString &jid, const QString &status) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/typing", {{"status", status}}),
           [this](const QJsonDocument &d){ emit chatActionDone("typing", d.object()); });
}
void ApiClient::subscribePresence(const QString &jid) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/subscribe", {}),
           [this](const QJsonDocument &d){ emit chatActionDone("subscribe", d.object()); });
}
void ApiClient::setChatArchived(const QString &jid, bool archived) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/archive", {{"archived", archived}}),
           [this](const QJsonDocument &d){ emit chatActionDone("archive", d.object()); });
}
void ApiClient::setChatPinned(const QString &jid, bool pinned) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/pin", {{"pinned", pinned}}),
           [this](const QJsonDocument &d){ emit chatActionDone("pin", d.object()); });
}
void ApiClient::setChatMuted(const QString &jid, bool muted, int durationSeconds) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/mute",
                    {{"muted", muted}, {"durationSeconds", durationSeconds}}),
           [this](const QJsonDocument &d){ emit chatActionDone("mute", d.object()); });
}
void ApiClient::clearChat(const QString &jid, bool remote) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/clear", {{"remote", remote}}),
           [this](const QJsonDocument &d){ emit chatActionDone("clear", d.object()); });
}
void ApiClient::deleteChat(const QString &jid, bool remote) {
    handleJson(postJson("/chats/" + enc(normalizeJid(jid)) + "/delete", {{"remote", remote}}),
           [this](const QJsonDocument &d){ emit chatActionDone("delete", d.object()); });
}

// ─── Messages ────────────────────────────────────────────────────────────────

void ApiClient::getMessages(const QString &jid, int limit, qint64 before, qint64 after, bool markRead) {
    QMap<QString,QString> q = {{"limit", QString::number(limit)},
                               {"markRead", markRead ? "1" : "0"}};
    if (before >= 0) q["before"] = QString::number(before);
    if (after >= 0) q["after"] = QString::number(after);
    QString j = normalizeJid(jid);
    handleJson(get("/messages/" + enc(j), q), [this, j](const QJsonDocument &d){
        QList<Message> out;
        for (const QJsonValue &v : d.array()) out.append(parseMessage(v.toObject()));
        emit messagesReady(j, out);
    });
}
void ApiClient::getMessage(const QString &jid, const QString &msgId) {
    handleJson(get("/messages/" + enc(normalizeJid(jid)) + "/" + enc(msgId)),
           [this](const QJsonDocument &d){ emit messageReady(parseMessage(d.object())); });
}
void ApiClient::getMedia(const QString &jid, const QString &msgId, bool thumb, bool download) {
    QMap<QString,QString> q;
    if (thumb) q["thumb"] = "1";
    if (download) q["download"] = "1";
    QNetworkReply *r = get("/messages/" + enc(normalizeJid(jid)) + "/" + enc(msgId) + "/media", q);
    QString j = normalizeJid(jid);
    QObject::connect(r, &QNetworkReply::finished, this, [this, r, j, msgId](){
        QByteArray b = r->readAll();
        QString mime = r->header(QNetworkRequest::ContentTypeHeader).toString();
        int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r->deleteLater();
        if (code == 200 && !b.isEmpty()) emit mediaReady(j, msgId, b, mime);
        else emit errorOccurred("Media unavailable", "MEDIA_UNAVAILABLE", code);
    });
}
void ApiClient::deleteMessage(const QString &jid, const QString &msgId, const QString &scope) {
    handleJson(postJson("/messages/" + enc(normalizeJid(jid)) + "/" + enc(msgId) + "/delete",
                    {{"scope", scope}}),
           [this](const QJsonDocument &d){ emit messageActionDone("delete", d.object()); });
}
void ApiClient::forwardMessage(const QString &fromJid, const QString &msgId, const QString &toJid) {
    handleJson(postJson("/messages/" + enc(normalizeJid(fromJid)) + "/" + enc(msgId) + "/forward",
                    {{"to", normalizeJid(toJid)}}),
           [this](const QJsonDocument &d){ emit messageActionDone("forward", d.object()); });
}
void ApiClient::reactMessage(const QString &jid, const QString &msgId, const QString &emoji) {
    handleJson(postJson("/messages/" + enc(normalizeJid(jid)) + "/" + enc(msgId) + "/react",
                    {{"emoji", emoji}}),
           [this](const QJsonDocument &d){ emit messageActionDone("react", d.object()); });
}
void ApiClient::editMessage(const QString &jid, const QString &msgId, const QString &text) {
    handleJson(postJson("/messages/" + enc(normalizeJid(jid)) + "/" + enc(msgId) + "/edit",
                    {{"text", text}}),
           [this](const QJsonDocument &d){ emit messageActionDone("edit", d.object()); });
}

// ─── Search / sync ───────────────────────────────────────────────────────────

void ApiClient::searchMessages(const QString &q, const QString &jid, int limit, int offset) {
    QMap<QString,QString> query = {{"q", q}, {"limit", QString::number(limit)},
                                   {"offset", QString::number(offset)}};
    if (!jid.isEmpty()) query["jid"] = normalizeJid(jid);
    handleJson(get("/search", query), [this](const QJsonDocument &d){
        QList<Message> out;
        for (const QJsonValue &v : d.array()) out.append(parseMessage(v.toObject()));
        emit searchReady(out);
    });
}
void ApiClient::sync(qint64 sinceMs, int limit) {
    handleJson(get("/sync", {{"since", QString::number(sinceMs)},
                         {"limit", QString::number(limit)}}),
           [this](const QJsonDocument &d){
        QJsonObject o = d.object();
        QList<Chat> chats;
        for (const QJsonValue &v : o.value("chats").toArray()) chats.append(parseChat(v.toObject()));
        QList<Message> msgs;
        for (const QJsonValue &v : o.value("messages").toArray()) msgs.append(parseMessage(v.toObject()));
        emit syncReady(o.value("nextSince").toVariant().toLongLong(),
                       o.value("hasMore").toBool(), chats, msgs);
    });
}

// ─── Send ────────────────────────────────────────────────────────────────────

void ApiClient::sendText(const QString &jid, const QString &text, const QString &quotedId) {
    QJsonObject body = {{"jid", normalizeJid(jid)}, {"text", text}};
    if (!quotedId.isEmpty()) body["quotedId"] = quotedId;
    handleJson(postJson("/send", body), [this](const QJsonDocument &d){ emit sendDone(d.object()); });
}
void ApiClient::sendMedia(const QString &jid, const QString &filePath,
                          const QString &mediaType, const QString &caption,
                          const QString &quotedId, bool ptt) {
    QFile *f = new QFile(filePath);
    if (!f->open(QIODevice::ReadOnly)) {
        emit errorOccurred("Cannot open file: " + filePath, "FILE_NOT_FOUND", 0);
        delete f;
        return;
    }
    QString type = mediaType.isEmpty() ? guessMediaType(filePath) : mediaType;
    QMap<QString,QString> fields = {{"jid", normalizeJid(jid)},
                                    {"mediaType", type},
                                    {"caption", caption},
                                    {"mimetype", guessMime(filePath)},
                                    {"fileName", QFileInfo(filePath).fileName()}};
    if (!quotedId.isEmpty()) fields["quotedId"] = quotedId;
    if (ptt) fields["ptt"] = "1";
    QNetworkReply *r = postMultipart("/send", fields, {f}, "file");
    QObject::connect(r, &QNetworkReply::finished, this, [this, r](){
        QJsonDocument d = QJsonDocument::fromJson(r->readAll());
        int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r->deleteLater();
        if (code == 200 && d.isObject()) emit sendDone(d.object());
        else {
            QString msg = d.isObject() ? d.object().value("error").toString("Send failed")
                                       : "Send failed";
            emit errorOccurred(msg, d.isObject() ? d.object().value("code").toString() : "SEND_FAILED", code);
        }
    });
}
void ApiClient::sendContact(const QString &jid, const QString &name, const QString &phone,
                            const QString &quotedId) {
    QJsonObject body = {{"jid", normalizeJid(jid)}, {"name", name}, {"phone", phone}};
    if (!quotedId.isEmpty()) body["quotedId"] = quotedId;
    handleJson(postJson("/send-contact", body), [this](const QJsonDocument &d){ emit sendDone(d.object()); });
}
void ApiClient::sendLocation(const QString &jid, double lat, double lng,
                             const QString &name, const QString &address,
                             const QString &quotedId) {
    QJsonObject body = {{"jid", normalizeJid(jid)},
                        {"latitude", lat}, {"longitude", lng}};
    if (!name.isEmpty()) body["name"] = name;
    if (!address.isEmpty()) body["address"] = address;
    if (!quotedId.isEmpty()) body["quotedId"] = quotedId;
    handleJson(postJson("/send-location", body), [this](const QJsonDocument &d){ emit sendDone(d.object()); });
}

// ─── Profile / contacts / presence ─────────────────────────────────────────

void ApiClient::getMe() {
    handleJson(get("/me"), [this](const QJsonDocument &d){ emit meReady(d.object()); });
}
void ApiClient::updateMyStatus(const QString &status) {
    handleJson(postJson("/me/status", {{"status", status}}),
           [this](const QJsonDocument &d){ emit profileActionDone("status", d.object()); });
}
void ApiClient::updateMyName(const QString &name) {
    handleJson(postJson("/me/name", {{"name", name}}),
           [this](const QJsonDocument &d){ emit profileActionDone("name", d.object()); });
}
void ApiClient::setMyPresence(const QString &status) {
    handleJson(postJson("/me/presence", {{"status", status}}),
           [this](const QJsonDocument &d){ emit profileActionDone("presence", d.object()); });
}
void ApiClient::getUser(const QString &jid) {
    handleJson(get("/user/" + enc(normalizeJid(jid))),
           [this](const QJsonDocument &d){ emit userReady(d.object()); });
}
void ApiClient::checkOnWhatsApp(const QString &phone) {
    handleJson(get("/check/" + enc(phone)), [this](const QJsonDocument &d){ emit checkReady(d.object()); });
}
void ApiClient::checkOnWhatsAppBatch(const QStringList &phones) {
    QJsonArray arr;
    for (const QString &p : phones) arr.append(p);
    handleJson(postJson("/check", {{"phones", arr}}),
           [this](const QJsonDocument &d){ emit checkBatchReady(d.array()); });
}
void ApiClient::getContacts(const QString &q) {
    QMap<QString,QString> query;
    if (!q.isEmpty()) query["q"] = q;
    handleJson(get("/contacts", query), [this](const QJsonDocument &d){
        QList<Contact> out;
        for (const QJsonValue &v : d.array()) out.append(parseContact(v.toObject()));
        emit contactsReady(out);
    });
}
void ApiClient::getBlocked() {
    handleJson(get("/blocked"), [this](const QJsonDocument &d){ emit blockedReady(d.array()); });
}
void ApiClient::setBlocked(const QString &jid, bool blocked) {
    handleJson(postJson((blocked ? "/block/" : "/unblock/") + enc(normalizeJid(jid)), {}),
           [this, blocked](const QJsonDocument &d){
               emit profileActionDone(blocked ? "block" : "unblock", d.object()); });
}
void ApiClient::getPresence(const QString &jid) {
    if (jid.isEmpty())
        handleJson(get("/presence"), [this](const QJsonDocument &d){ emit presenceReady(d.object()); });
    else
        handleJson(get("/presence/" + enc(normalizeJid(jid))),
               [this](const QJsonDocument &d){ emit presenceReady(d.object()); });
}
void ApiClient::getProfilePic(const QString &jid, bool preview) {
    QNetworkReply *r = get("/profile-pic/" + enc(normalizeJid(jid)),
                           preview ? QMap<QString,QString>{{"preview","1"}} : QMap<QString,QString>{});
    QString j = normalizeJid(jid);
    QObject::connect(r, &QNetworkReply::finished, this, [this, r, j](){
        QByteArray b = r->readAll();
        int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r->deleteLater();
        if (code == 200 && !b.isEmpty()) emit profilePicReady(j, b);
        else emit errorOccurred("No profile picture", "NO_PROFILE_PIC", code);
    });
}

// ─── Groups ──────────────────────────────────────────────────────────────────

void ApiClient::getGroups(int limit, int offset) {
    handleJson(get("/groups", {{"limit", QString::number(limit)},
                           {"offset", QString::number(offset)}}),
           [this](const QJsonDocument &d){
        QList<Chat> out;
        for (const QJsonValue &v : d.array()) out.append(parseChat(v.toObject()));
        emit groupsReady(out);
    });
}
void ApiClient::createGroup(const QString &subject, const QStringList &participants) {
    QJsonArray arr;
    for (const QString &p : participants) arr.append(normalizeJid(p));
    handleJson(postJson("/groups", {{"subject", subject}, {"participants", arr}}),
           [this](const QJsonDocument &d){ emit groupActionDone("create", d.object()); });
}
void ApiClient::joinGroup(const QString &codeOrLink) {
    handleJson(postJson("/groups/join", {{"code", codeOrLink}}),
           [this](const QJsonDocument &d){ emit groupActionDone("join", d.object()); });
}
void ApiClient::getGroup(const QString &jid) {
    handleJson(get("/groups/" + enc(normalizeJid(jid))),
           [this](const QJsonDocument &d){ emit groupReady(parseGroup(d.object())); });
}
void ApiClient::updateGroupParticipants(const QString &jid, const QStringList &participants,
                                        const QString &action) {
    QJsonArray arr;
    for (const QString &p : participants) arr.append(normalizeJid(p));
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/participants",
                    {{"action", action}, {"participants", arr}}),
           [this](const QJsonDocument &d){ emit groupActionDone("participants", d.object()); });
}
void ApiClient::leaveGroup(const QString &jid) {
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/leave", {}),
           [this](const QJsonDocument &d){ emit groupActionDone("leave", d.object()); });
}
void ApiClient::getGroupInvite(const QString &jid) {
    handleJson(get("/groups/" + enc(normalizeJid(jid)) + "/invite"),
           [this](const QJsonDocument &d){ emit groupActionDone("invite", d.object()); });
}
void ApiClient::revokeGroupInvite(const QString &jid) {
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/invite/revoke", {}),
           [this](const QJsonDocument &d){ emit groupActionDone("revoke-invite", d.object()); });
}
void ApiClient::updateGroupSubject(const QString &jid, const QString &subject) {
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/subject", {{"subject", subject}}),
           [this](const QJsonDocument &d){ emit groupActionDone("subject", d.object()); });
}
void ApiClient::updateGroupDescription(const QString &jid, const QString &description) {
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/description", {{"description", description}}),
           [this](const QJsonDocument &d){ emit groupActionDone("description", d.object()); });
}
void ApiClient::setGroupSetting(const QString &jid, const QString &setting) {
    handleJson(postJson("/groups/" + enc(normalizeJid(jid)) + "/settings", {{"setting", setting}}),
           [this](const QJsonDocument &d){ emit groupActionDone("settings", d.object()); });
}
