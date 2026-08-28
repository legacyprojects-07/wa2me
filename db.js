'use strict';

const { Pool } = require('pg');

let pool;

/**
 * Initialize PostgreSQL connection pool.
 * Expects DATABASE_URL env var (auto-set by Render when linked to a PostgreSQL service).
 */
async function init() {
  const connectionString = process.env.DATABASE_URL;
  if (!connectionString) throw new Error('DATABASE_URL environment variable is required');

  // Render internal connections don't need SSL; external (Neon, etc.) do.
  // Detect by checking if hostname contains 'render' or 'internal'.
  const needsSSL = !connectionString.includes('internal') && !connectionString.includes('render');

  pool = new Pool({
    connectionString,
    ssl: needsSSL ? { rejectUnauthorized: false } : false,
    max: 5,
    idleTimeoutMillis: 30000,
    connectionTimeoutMillis: 15000,
  });

  pool.on('error', (err) => {
    console.error('[DB] Pool error:', err.message);
  });

  // Test connection
  const client = await pool.connect();
  console.log('[DB] Connected to PostgreSQL');
  client.release();

  await migrate();
}

function getPool() {
  if (!pool) throw new Error('Database not initialized');
  return pool;
}

async function close() {
  if (pool) {
    await pool.end();
    pool = null;
  }
}

// ─── Schema ──────────────────────────────────────────────────────────────────

async function migrate() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS chats (
      jid             TEXT PRIMARY KEY,
      name            TEXT NOT NULL DEFAULT '',
      is_group        BOOLEAN NOT NULL DEFAULT FALSE,
      unread_count    INTEGER NOT NULL DEFAULT 0,
      last_message_ts BIGINT NOT NULL DEFAULT 0,
      last_message    TEXT NOT NULL DEFAULT '',
      created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
      updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
    );

    CREATE TABLE IF NOT EXISTS contacts (
      jid          TEXT PRIMARY KEY,
      name         TEXT NOT NULL DEFAULT '',
      notify       TEXT NOT NULL DEFAULT '',
      phone_number TEXT NOT NULL DEFAULT '',
      is_saved     BOOLEAN NOT NULL DEFAULT FALSE,
      updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
    );

    CREATE TABLE IF NOT EXISTS messages (
      id           TEXT NOT NULL,
      chat_jid     TEXT NOT NULL,
      sender_jid   TEXT NOT NULL DEFAULT '',
      sender_name  TEXT NOT NULL DEFAULT '',
      from_me      BOOLEAN NOT NULL DEFAULT FALSE,
      timestamp    BIGINT NOT NULL DEFAULT 0,
      text         TEXT NOT NULL DEFAULT '',
      media_type   TEXT,
      has_media    BOOLEAN NOT NULL DEFAULT FALSE,
      media_path   TEXT,
      media_mime   TEXT,
      status       TEXT NOT NULL DEFAULT 'received',
      created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
      PRIMARY KEY (chat_jid, id)
    );

    CREATE INDEX IF NOT EXISTS idx_messages_chat_ts ON messages(chat_jid, timestamp);
    CREATE INDEX IF NOT EXISTS idx_chats_last_ts ON chats(last_message_ts DESC);
  `);
  console.log('[DB] Schema migrated');
}

// ─── Contacts ────────────────────────────────────────────────────────────────

async function upsertContact(c) {
  if (!c || !c.id) return;
  await pool.query(`
    INSERT INTO contacts (jid, name, notify, phone_number, is_saved, updated_at)
    VALUES ($1, $2, $3, $4, $5, NOW())
    ON CONFLICT(jid) DO UPDATE SET
      name         = COALESCE(NULLIF(EXCLUDED.name, ''), contacts.name),
      notify       = COALESCE(NULLIF(EXCLUDED.notify, ''), contacts.notify),
      phone_number = COALESCE(NULLIF(EXCLUDED.phone_number, ''), contacts.phone_number),
      is_saved     = CASE WHEN EXCLUDED.is_saved THEN TRUE ELSE contacts.is_saved END,
      updated_at   = NOW()
  `, [c.id, c.name || '', c.notify || '', c.phoneNumber || '', Boolean(c.isSaved)]);
}

async function getContact(jid) {
  const { rows } = await pool.query('SELECT * FROM contacts WHERE jid = $1', [jid]);
  return rows[0] || null;
}

async function getAllContacts() {
  const { rows } = await pool.query('SELECT * FROM contacts ORDER BY name');
  return rows;
}

async function resolveName(jid, fallback) {
  const c = await getContact(jid);
  if (c) {
    if (c.name && c.name.trim()) return c.name.trim();
    if (c.notify && c.notify.trim()) return c.notify.trim();
  }
  return fallback || (jid ? jid.replace(/@.*/, '') : '');
}

async function reResolveAllChatNames() {
  const { rows: chats } = await pool.query('SELECT jid, name FROM chats');
  for (const chat of chats) {
    const newName = await resolveName(chat.jid, chat.name);
    if (newName !== chat.name) {
      await pool.query("UPDATE chats SET name = $1, updated_at = NOW() WHERE jid = $2", [newName, chat.jid]);
    }
  }
}

// ─── Chats ───────────────────────────────────────────────────────────────────

async function upsertChat(chat, { isInitialSync = false, fromEvent = false, forceName = false } = {}) {
  if (!chat || !chat.id) return;

  const existing = await getChat(chat.id);
  const name = await resolveName(chat.id, chat.name || chat.subject || (existing && existing.name) || '');

  let unreadCount;
  if (isInitialSync || fromEvent) {
    if (chat.unreadCount !== undefined && chat.unreadCount !== null) {
      unreadCount = chat.unreadCount;
    } else if (chat.unread_count !== undefined) {
      unreadCount = chat.unread_count;
    } else {
      unreadCount = existing ? existing.unread_count : 0;
    }
  } else {
    unreadCount = existing ? existing.unread_count : 0;
  }

  const lastMessageTs = parseTimestamp(chat.conversationTimestamp || chat.lastMessageTs);

  await pool.query(`
    INSERT INTO chats (jid, name, is_group, unread_count, last_message_ts, last_message, updated_at)
    VALUES ($1, $2, $3, $4, $5, $6, NOW())
    ON CONFLICT(jid) DO UPDATE SET
      name            = CASE WHEN $7 THEN $2
                          ELSE COALESCE(NULLIF(EXCLUDED.name, ''), chats.name) END,
      is_group        = EXCLUDED.is_group,
      unread_count    = EXCLUDED.unread_count,
      last_message_ts = GREATEST(chats.last_message_ts, EXCLUDED.last_message_ts),
      last_message    = CASE WHEN EXCLUDED.last_message_ts >= chats.last_message_ts
                          THEN EXCLUDED.last_message ELSE chats.last_message END,
      updated_at      = NOW()
  `, [chat.id, name, isGroup(chat.id), unreadCount, lastMessageTs, chat.lastMessage || '', forceName]);
}

async function getChat(jid) {
  const { rows } = await pool.query('SELECT * FROM chats WHERE jid = $1', [jid]);
  return rows[0] || null;
}

async function listChats({ savedOnly = true, limit = 200, offset = 0 } = {}) {
  let sql = `
    SELECT c.*,
           COALESCE(ct.name, '')   AS contact_name,
           COALESCE(ct.notify, '') AS contact_notify
    FROM chats c
    LEFT JOIN contacts ct ON ct.jid = c.jid
    WHERE c.last_message_ts > 0
  `;

  if (savedOnly) {
    sql += ` AND (c.is_group = TRUE OR ct.is_saved = TRUE OR ct.name != '' OR ct.notify != '')`;
  }

  sql += ` ORDER BY c.last_message_ts DESC LIMIT $1 OFFSET $2`;
  const { rows } = await pool.query(sql, [limit, offset]);
  return rows;
}

async function setUnreadCount(jid, count) {
  await pool.query("UPDATE chats SET unread_count = $1, updated_at = NOW() WHERE jid = $2", [count, jid]);
}

async function incrementUnread(jid) {
  await pool.query("UPDATE chats SET unread_count = unread_count + 1, updated_at = NOW() WHERE jid = $1", [jid]);
}

async function recalculateLastMessage(chatJid) {
  const { rows } = await pool.query(
    'SELECT text, media_type, timestamp FROM messages WHERE chat_jid = $1 ORDER BY timestamp DESC LIMIT 1',
    [chatJid]
  );

  if (rows.length > 0) {
    const row = rows[0];
    const lastMessage = row.text || (row.media_type ? `[${row.media_type}]` : '');
    await pool.query(
      "UPDATE chats SET last_message = $1, last_message_ts = $2, updated_at = NOW() WHERE jid = $3",
      [lastMessage, row.timestamp, chatJid]
    );
  } else {
    await pool.query(
      "UPDATE chats SET last_message = '', last_message_ts = 0, updated_at = NOW() WHERE jid = $1",
      [chatJid]
    );
  }
}

// ─── Messages ────────────────────────────────────────────────────────────────

async function insertMessage(m) {
  await pool.query(`
    INSERT INTO messages
      (id, chat_jid, sender_jid, sender_name, from_me, timestamp, text, media_type, has_media, media_path, media_mime, status)
    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)
    ON CONFLICT (chat_jid, id) DO NOTHING
  `, [
    m.id, m.chatJid, m.senderJid || '', m.senderName || '', Boolean(m.fromMe),
    m.timestamp, m.text || '', m.mediaType || null, Boolean(m.hasMedia),
    m.mediaPath || null, m.mediaMime || null, m.status || 'received',
  ]);
}

async function updateMessageContent(chatJid, msgId, text, mediaType, hasMedia, mimetype) {
  await pool.query(
    'UPDATE messages SET text = $1, media_type = $2, has_media = $3, media_mime = $4 WHERE chat_jid = $5 AND id = $6',
    [text, mediaType, Boolean(hasMedia), mimetype || null, chatJid, msgId]
  );
}

async function updateMessageMedia(chatJid, msgId, mediaPath, mediaMime) {
  await pool.query(
    'UPDATE messages SET media_path = $1, media_mime = $2 WHERE chat_jid = $3 AND id = $4',
    [mediaPath, mediaMime, chatJid, msgId]
  );
}

async function getMessage(chatJid, msgId) {
  const { rows } = await pool.query(
    'SELECT * FROM messages WHERE chat_jid = $1 AND id = $2', [chatJid, msgId]
  );
  return rows[0] || null;
}

async function getMessages(chatJid, { limit = 100, before = null } = {}) {
  if (before) {
    const { rows } = await pool.query(
      'SELECT * FROM messages WHERE chat_jid = $1 AND timestamp < $2 ORDER BY timestamp ASC LIMIT $3',
      [chatJid, before, limit]
    );
    return rows;
  }
  const { rows } = await pool.query(
    'SELECT * FROM messages WHERE chat_jid = $1 ORDER BY timestamp ASC LIMIT $2',
    [chatJid, limit]
  );
  return rows;
}

async function getLatestMessages(chatJid, limit = 100) {
  const { rows } = await pool.query(`
    SELECT * FROM (
      SELECT * FROM messages WHERE chat_jid = $1 ORDER BY timestamp DESC LIMIT $2
    ) sub ORDER BY timestamp ASC
  `, [chatJid, limit]);
  return rows;
}

async function getMessageCount(chatJid) {
  const { rows } = await pool.query('SELECT COUNT(*)::int AS cnt FROM messages WHERE chat_jid = $1', [chatJid]);
  return rows[0]?.cnt || 0;
}

async function deleteMessage(chatJid, msgId) {
  await pool.query('DELETE FROM messages WHERE chat_jid = $1 AND id = $2', [chatJid, msgId]);
  await recalculateLastMessage(chatJid);
}

async function pruneOldMessages(chatJid, maxMessages = 1000) {
  const count = await getMessageCount(chatJid);
  if (count <= maxMessages) return 0;

  const toDelete = count - maxMessages;
  await pool.query(`
    DELETE FROM messages WHERE chat_jid = $1 AND id IN (
      SELECT id FROM messages WHERE chat_jid = $1 ORDER BY timestamp ASC LIMIT $2
    )
  `, [chatJid, toDelete]);

  await recalculateLastMessage(chatJid);
  return toDelete;
}

// ─── Stats ───────────────────────────────────────────────────────────────────

async function getStats() {
  const [chats, contacts, messages] = await Promise.all([
    pool.query('SELECT COUNT(*)::int AS c FROM chats'),
    pool.query('SELECT COUNT(*)::int AS c FROM contacts'),
    pool.query('SELECT COUNT(*)::int AS c FROM messages'),
  ]);
  return {
    chats: chats.rows[0].c,
    contacts: contacts.rows[0].c,
    messages: messages.rows[0].c,
  };
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
  init, getPool, close,
  upsertContact, getContact, getAllContacts, resolveName, reResolveAllChatNames,
  upsertChat, getChat, listChats, setUnreadCount, incrementUnread, recalculateLastMessage,
  insertMessage, updateMessageContent, updateMessageMedia, getMessage, getMessages, getLatestMessages, getMessageCount, deleteMessage, pruneOldMessages,
  getStats,
};
