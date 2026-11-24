#pragma once
// Models mirroring serializers.js (OpenKnot REST API payloads).
// Field names match the JSON exactly so QJsonDocument parsing is trivial.

#include <QString>
#include <QDateTime>

struct Chat {
    QString jid;
    QString id;              // legacy v5 alias of jid
    QString name;
    bool isGroup = false;
    int unreadCount = 0;
    qint64 lastMessageTs = 0; // unix seconds
    QString lastMessage;
    bool archived = false;
    bool pinned = false;
    bool muted = false;
    qint64 muteUntil = 0;
};

struct QuotedMessage {
    QString id;
    bool fromMe = false;
    QString senderName;
    QString mediaType;
    bool deleted = false;
    QString text;
};

struct Reaction {
    QString senderJid;
    QString emoji;
    bool fromMe = false;
};

struct Message {
    QString id;
    QString chatJid;
    bool fromMe = false;
    QString senderJid;
    QString senderName;
    qint64 timestamp = 0; // unix seconds
    QString text;
    QString mediaType;    // image|video|audio|document|sticker|null
    bool hasMedia = false;
    QString mediaUrl;     // e.g. /messages/<jid>/<id>/media
    QString mediaMime;
    QString fileName;
    int mediaSeconds = 0;
    bool isPtt = false;
    QString status;       // pending|sent|delivered|read|played|received|error
    bool deleted = false;
    qint64 deletedAt = 0;
    bool edited = false;
    QString quotedId;
    QuotedMessage quoted;
    QList<Reaction> reactions;
};

struct Contact {
    QString jid;
    QString name;
    QString notify;
    QString phoneNumber;
    bool isSaved = false;
};

struct GroupParticipant {
    QString jid;
    QString phoneNumber;
    QString name;
    QString admin; // null | admin | superadmin
    bool isAdmin = false;
    bool isSuperAdmin = false;
};

struct GroupInfo {
    QString jid;
    QString subject;
    QString description;
    QString owner;
    qint64 createdAt = 0;
    bool isAnnounce = false;
    bool isLocked = false;
    bool isCommunity = false;
    int size = 0;
    QList<GroupParticipant> participants;
};
