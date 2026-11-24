#pragma once
// Models mirroring serializers.js (OpenKnot REST API payloads).
// Field names match the JSON exactly so QJsonDocument parsing is trivial.

#include <QString>
#include <QDateTime>

struct Chat {
    QString jid;
    QString id;              // legacy v5 alias of jid
    QString name;
    bool isGroup;
    int unreadCount;
    qint64 lastMessageTs;  // unix seconds
    QString lastMessage;
    bool archived;
    bool pinned;
    bool muted;
    qint64 muteUntil;
    Chat()
        : isGroup(false),
          unreadCount(0),
          lastMessageTs(0),
          archived(false),
          pinned(false),
          muted(false),
          muteUntil(0) {}
};

struct QuotedMessage {
    QString id;
    bool fromMe;
    QString senderName;
    QString mediaType;
    bool deleted;
    QString text;
    QuotedMessage() : fromMe(false), deleted(false) {}
};

struct Reaction {
    QString senderJid;
    QString emoji;
    bool fromMe;
    Reaction() : fromMe(false) {}
};

struct Message {
    QString id;
    QString chatJid;
    bool fromMe;
    QString senderJid;
    QString senderName;
    qint64 timestamp;  // unix seconds
    QString text;
    QString mediaType;    // image|video|audio|document|sticker|null
    bool hasMedia;
    QString mediaUrl;     // e.g. /messages/<jid>/<id>/media
    QString mediaMime;
    QString fileName;
    int mediaSeconds;
    bool isPtt;
    QString status;       // pending|sent|delivered|read|played|received|error
    bool deleted;
    qint64 deletedAt;
    bool edited;
    QString quotedId;
    QuotedMessage quoted;
    QList<Reaction> reactions;
    Message()
        : fromMe(false),
          timestamp(0),
          hasMedia(false),
          mediaSeconds(0),
          isPtt(false),
          deleted(false),
          deletedAt(0),
          edited(false) {}
};

struct Contact {
    QString jid;
    QString name;
    QString notify;
    QString phoneNumber;
    bool isSaved;
    Contact() : isSaved(false) {}
};

struct GroupParticipant {
    QString jid;
    QString phoneNumber;
    QString name;
    QString admin; // null | admin | superadmin
    bool isAdmin;
    bool isSuperAdmin;
    GroupParticipant() : isAdmin(false), isSuperAdmin(false) {}
};

struct GroupInfo {
    QString jid;
    QString subject;
    QString description;
    QString owner;
    qint64 createdAt;
    bool isAnnounce;
    bool isLocked;
    bool isCommunity;
    int size;
    QList<GroupParticipant> participants;
    GroupInfo()
        : createdAt(0),
          isAnnounce(false),
          isLocked(false),
          isCommunity(false),
          size(0) {}
};
