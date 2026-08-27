'use strict';

const Database = require('better-sqlite3');
const path = require('path');
const fs = require('fs');

let db;

function init(dbPath) {
  const dir = path.dirname(dbPath);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });

  db = new Database(dbPath);
  db.pragma('journal_mode = WAL');
  db.pragma('synchronous = NORMAL');
  db.pragma('foreign_keys = ON');
  migrate();
  return db;
}

function getDb() {
  if (!db) throw new Error('Database not initialized');
  return db;
}

function close() {
  if (db) {
    db.close();
    db = null;
  }
}

// ─── Schema ──────────────────────────────────────────────────────────────────

function migrate() {
  db.exec(`
    CREATE TABLE IF NOT EXISTS chats (
      jid             TEXT PRIMARY KEY,
      name            TEXT NOT NULL DEFAULT '',
      is_group        INTEGER NOT NULL DEFAULT 0,
      unread_count    INTEGER NOT NULL DEFAULT 0,
      last_message_ts INTEGER NOT NULL DEFAULT 0,
      last_message    TEXT NOT NULL DEFAULT '',
      created_at      TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
    );

    CREATE TABLE IF NOT EXISTS contacts (
      jid          TEXT PRIMARY KEY,
      name         TEXT NOT NULL DEFAULT '',
      notify       TEXT NOT NULL DEFAULT '',
      phone_number TEXT NOT NULL DEFAULT '',
      is_saved     INTEGER NOT NULL DEFAULT 0,
      updated_at   TEXT NOT NULL DEFAULT (datetime('now'))
    );

    CREATE TABLE IF NOT EXISTS messages (
      id           TEXT NOT NULL,
      chat_jid     TEXT NOT NULL,
      sender_jid   TEXT NOT NULL DEFAULT '',
      sender_name  TEXT NOT NULL DEFAULT '',
      from_me      INTEGER NOT NULL DEFAULT 0,
      timestamp    INTEGER NOT NULL DEFAULT 0,
      text         TEXT NOT NULL DEFAULT '',
      media_type   TEXT,
      has_media    INTEGER NOT NULL DEFAULT 0,
      media_path   TEXT,
      media_mime   TEXT,
      status       TEXT NOT NULL DEFAULT 'received',
      created_at   TEXT NOT NULL DEFAULT (datetime('now')),
      PRIMARY KEY (chat_jid, id)
    );

    CREATE INDEX IF NOT EXISTS idx_messages_chat_ts ON messages(chat_jid, timestamp);
    CREATE INDEX IF NOT EXISTS idx_chats_last_ts ON chats(last_message_ts DESC);
  `);
}

// ─── Contacts ────────────────────────────────────────────────────────────────

const UPSERT_CONTACT = `
  INSERT INTO contacts (jid, name, notify, phone_number, is_saved, updated_at)
  VALUES (@jid, @name, @notify, @phone_number, @is_saved, datetime('now'))
  ON CONFLICT(jid) DO UPDATE SET
    name         = COALESCE(NULLIF(@name, ''), contacts.name),
    notify       = COALESCE(NULLIF(@notify, ''), contacts.notify),
    phone_number = COALESCE(NULLIF(@phone_number, ''), contacts.phone_number),
    is_saved     = CASE WHEN @is_saved = 1 THEN 1 ELSE contacts.is_saved END,
    updated_at   = datetime('now')
`;

function upsertContact(c) {
  if (!c || !c.id) return;
  db.prepare(UPSERT_CONTACT).run({
    jid: c.id,
    name: c.name || '',
    notify: c.notify || '',
    phone_number: c.phoneNumber || '',
    is_saved: c.isSaved ? 1 : 0,
  });
}

function getContact(jid) {
  return db.prepare('SELECT * FROM contacts WHERE jid = ?').get(jid) || null;
}

function getAllContacts() {
  return db.prepare('SELECT * FROM contacts ORDER BY name').all();
}

function resolveName(jid, fallback) {
  const c = getContact(jid);
  if (c) {
    if (c.name && c.name.trim()) return c.name.trim();
    if (c.notify && c.notify.trim()) return c.notify.trim();
  }
  return fallback || (jid ? jid.replace(/@.*/, '') : '');
}

function reResolveAllChatNames() {
  const chats = db.prepare('SELECT jid, name FROM chats').all();
  const update = db.prepare("UPDATE chats SET name = ?, updated_at = datetime('now') WHERE jid = ?");
  const tx = db.transaction(() => {
    for (const chat of chats) {
      const newName = resolveName(chat.jid, chat.name);
      if (newName !== chat.name) update.run(newName, chat.jid);
    }
  });
  tx();
}

// ─── Chats ───────────────────────────────────────────────────────────────────

const UPSERT_CHAT = `
  INSERT INTO chats (jid, name, is_group, unread_count, last_message_ts, last_message, updated_at)
  VALUES (@jid, @name, @is_group, @unread_count, @last_message_ts, @last_message, datetime('now'))
  ON CONFLICT(jid) DO UPDATE SET
    name            = CASE WHEN @force_name = 1 THEN @name
                        ELSE COALESCE(NULLIF(@name, ''), chats.name) END,
    is_group        = @is_group,
    unread_count    = @unread_count,
    last_message_ts = MAX(chats.last_message_ts, @last_message_ts),
    last_message    = CASE WHEN @last_message_ts >= chats.last_message_ts
                        THEN @last_message ELSE chats.last_message END,
    updated_at      = datetime('now')
`;

function upsertChat(chat, { isInitialSync = false, forceName = false } = {}) {
  if (!chat || !chat.id) return;

  const existing = db.prepare('SELECT * FROM chats WHERE jid = ?').get(chat.id);
  const name = resolveName(chat.id, chat.name || chat.subject || (existing && existing.name) || '');

  let unreadCount;
  if (isInitialSync) {
    unreadCount = chat.unreadCount ?? (existing ? existing.unread_count : 0);
  } else {
    unreadCount = existing ? existing.unread_count : 0;
  }

  db.prepare(UPSERT_CHAT).run({
    jid: chat.id,
    name: name,
    is_group: isGroup(chat.id) ? 1 : 0,
    unread_count: unreadCount,
    last_message_ts: parseTimestamp(chat.conversationTimestamp || chat.lastMessageTs),
    last_message: chat.lastMessage || '',
    force_name: forceName ? 1 : 0,
  });
}

function getChat(jid) {
  return db.prepare('SELECT * FROM chats WHERE jid = ?').get(jid) || null;
}

function listChats({ savedOnly = true, limit = 200, offset = 0 } = {}) {
  let sql = `
    SELECT c.*,
           COALESCE(ct.name, '')   AS contact_name,
           COALESCE(ct.notify, '') AS contact_notify
    FROM chats c
    LEFT JOIN contacts ct ON ct.jid = c.jid
    WHERE c.last_message_ts > 0
  `;

  if (savedOnly) {
    sql += ` AND (c.is_group = 1 OR (ct.is_saved = 1 OR ct.name != '' OR ct.notify != ''))`;
  }

  sql += ` ORDER BY c.last_message_ts DESC LIMIT @limit OFFSET @offset`;
  return db.prepare(sql).all({ limit, offset });
}

function setUnreadCount(jid, count) {
  db.prepare("UPDATE chats SET unread_count = ?, updated_at = datetime('now') WHERE jid = ?")
    .run(count, jid);
}

function incrementUnread(jid) {
  db.prepare("UPDATE chats SET unread_count = unread_count + 1, updated_at = datetime('now') WHERE jid = ?")
    .run(jid);
}

// ─── Messages ────────────────────────────────────────────────────────────────

const INSERT_MESSAGE = `
  INSERT OR IGNORE INTO messages
    (id, chat_jid, sender_jid, sender_name, from_me, timestamp, text, media_type, has_media, media_path, media_mime, status)
  VALUES
    (@id, @chat_jid, @sender_jid, @sender_name, @from_me, @timestamp, @text, @media_type, @has_media, @media_path, @media_mime, @status)
`;

function insertMessage(m) {
  db.prepare(INSERT_MESSAGE).run({
    id: m.id,
    chat_jid: m.chatJid,
    sender_jid: m.senderJid || '',
    sender_name: m.senderName || '',
    from_me: m.fromMe ? 1 : 0,
    timestamp: m.timestamp,
    text: m.text || '',
    media_type: m.mediaType || null,
    has_media: m.hasMedia ? 1 : 0,
    media_path: m.mediaPath || null,
    media_mime: m.mediaMime || null,
    status: m.status || 'received',
  });
}

function updateMessageMedia(chatJid, msgId, mediaPath, mediaMime) {
  db.prepare('UPDATE messages SET media_path = ?, media_mime = ? WHERE chat_jid = ? AND id = ?')
    .run(mediaPath, mediaMime, chatJid, msgId);
}

function getMessage(chatJid, msgId) {
  return db.prepare('SELECT * FROM messages WHERE chat_jid = ? AND id = ?').get(chatJid, msgId) || null;
}

function getMessages(chatJid, { limit = 100, before = null } = {}) {
  if (before) {
    return db.prepare(
      'SELECT * FROM messages WHERE chat_jid = ? AND timestamp < ? ORDER BY timestamp ASC LIMIT ?'
    ).all(chatJid, before, limit);
  }
  return db.prepare(
    'SELECT * FROM messages WHERE chat_jid = ? ORDER BY timestamp ASC LIMIT ?'
  ).all(chatJid, limit);
}

function getLatestMessages(chatJid, limit = 100) {
  return db.prepare(
    'SELECT * FROM messages WHERE chat_jid = ? ORDER BY timestamp DESC LIMIT ?'
  ).all(chatJid, limit).reverse();
}

function getMessageCount(chatJid) {
  const row = db.prepare('SELECT COUNT(*) AS cnt FROM messages WHERE chat_jid = ?').get(chatJid);
  return row ? row.cnt : 0;
}

function deleteMessage(chatJid, msgId) {
  db.prepare('DELETE FROM messages WHERE chat_jid = ? AND id = ?').run(chatJid, msgId);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

function parseTimestamp(ts) {
  if (!ts) return Math.floor(Date.now() / 1000);
  if (typeof ts === 'number') return ts;
  if (typeof ts === 'string') return parseInt(ts, 10) || Math.floor(Date.now() / 1000);
  if (typeof ts === 'object' && ts.low !== undefined) return ts.low;
  return Math.floor(Date.now() / 1000);
}

function isGroup(jid) {
  return jid && jid.endsWith('@g.us');
}

module.exports = {
  init, getDb, close,
  upsertContact, getContact, getAllContacts, resolveName, reResolveAllChatNames,
  upsertChat, getChat, listChats, setUnreadCount, incrementUnread,
  insertMessage, updateMessageMedia, getMessage, getMessages, getLatestMessages, getMessageCount, deleteMessage,
};
