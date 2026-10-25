'use strict';

const { Pool } = require('pg');
const {
  parseTimestamp,
  isGroup,
  decodeRawMessage,
} = require('./helpers');

let pool;

/**
 * Initialize PostgreSQL connection pool.
 * Expects DATABASE_URL env var (auto-set by Render when linked to a PostgreSQL service).
 */
async function init() {
  const connectionString = process.env.DATABASE_URL;
  if (!connectionString) throw new Error('DATABASE_URL environment variable is required');

  // Internal container/DB hostnames (Render internal, Railway .internal,
  // Docker `db`) have no public DNS suffix and usually do not use TLS.
  // Public hosts (Neon, Supabase, external Render URLs) need SSL.
  let needsSSL = !connectionString.includes('internal') && !connectionString.includes('render');
  try {
    const u = new URL(connectionString);
    const host = u.hostname;
    const sslmode = u.searchParams.get('sslmode');
    if (sslmode === 'disable') needsSSL = false;
    else if (sslmode === 'require' || sslmode === 'verify-full') needsSSL = true;
    else needsSSL = host.includes('.') && !host.endsWith('.internal') && !host.endsWith('.local');
  } catch {}

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

  // Additive migrations. Safe to run on every boot.
  await pool.query(`
    ALTER TABLE chats ADD COLUMN IF NOT EXISTS is_archived BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE chats ADD COLUMN IF NOT EXISTS is_pinned   BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE chats ADD COLUMN IF NOT EXISTS is_muted    BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE chats ADD COLUMN IF NOT EXISTS mute_until  BIGINT  NOT NULL DEFAULT 0;

    ALTER TABLE messages ADD COLUMN IF NOT EXISTS deleted       BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS deleted_at    BIGINT;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW();
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS quoted_id     TEXT;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS media_name    TEXT;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS media_seconds INTEGER;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS is_ptt        BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS edited        BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE messages ADD COLUMN IF NOT EXISTS raw_json      JSONB;

    CREATE TABLE IF NOT EXISTS reactions (
      chat_jid    TEXT NOT NULL,
      message_id  TEXT NOT NULL,
      sender_jid  TEXT NOT NULL DEFAULT '',
      emoji       TEXT NOT NULL,
      from_me     BOOLEAN NOT NULL DEFAULT FALSE,
      updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
      PRIMARY KEY (chat_jid, message_id, sender_jid)
    );

    CREATE INDEX IF NOT EXISTS idx_chats_updated           ON chats(updated_at);
    CREATE INDEX IF NOT EXISTS idx_messages_updated        ON messages(updated_at);
    CREATE INDEX IF NOT EXISTS idx_messages_chat_updated   ON messages(chat_jid, updated_at);
    CREATE INDEX IF NOT EXISTS idx_messages_chat_deleted   ON messages(chat_jid, deleted);
    CREATE INDEX IF NOT EXISTS idx_reactions_lookup        ON reactions(chat_jid, message_id);
  `);

  console.log('[DB] Schema migrated');
}

// Message columns for client-facing queries. raw_json is intentionally excluded
// (it can be large) and fetched only by getRawMessage().
const MSG_COLS = `
  id, chat_jid, sender_jid, sender_name, from_me, timestamp, text,
  media_type, has_media, media_path, media_mime, media_name, media_seconds,
  is_ptt, status, deleted, deleted_at, updated_at, quoted_id, edited
`;

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
  const { rows } = await pool.query('SELECT * FROM contacts ORDER BY name, notify');
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
    if (newName && newName !== chat.name) {
      await pool.query('UPDATE chats SET name = $1, updated_at = NOW() WHERE jid = $2', [newName, chat.jid]);
    }
  }
}

// ─── Chats ───────────────────────────────────────────────────────────────────

async function upsertChat(chat, { isInitialSync = false, fromEvent = false, forceName = false, preserveUnread = false } = {}) {
  if (!chat || !chat.id) return;

  const existing = await getChat(chat.id);
  const name = await resolveName(chat.id, chat.name || chat.subject || (existing && existing.name) || '');

  let unreadCount;
  if (preserveUnread) {
    // History sync: chat row already contains WhatsApp's accurate unread count.
    unreadCount = existing ? existing.unread_count : 0;
  } else if (isInitialSync || fromEvent) {
    const incoming = chat.unreadCount !== undefined && chat.unreadCount !== null
      ? chat.unreadCount
      : chat.unread_count;
    if (incoming === -1) {
      // Baileys uses -1 to mean "mark this chat unread".
      unreadCount = (existing ? existing.unread_count : 0) + 1;
    } else if (incoming !== undefined && incoming !== null) {
      unreadCount = Math.max(0, Number(incoming) || 0);
    } else {
      unreadCount = existing ? existing.unread_count : 0;
    }
  } else {
    unreadCount = existing ? existing.unread_count : 0;
  }

  const chatTs = chat.conversationTimestamp || chat.lastMessageTs;
  const lastMessageTs = chatTs ? parseTimestamp(chatTs) : (existing ? existing.last_message_ts : 0);
  const incomingLast = typeof chat.lastMessage === 'string' ? chat.lastMessage : '';

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
                          THEN COALESCE(NULLIF(EXCLUDED.last_message, ''), chats.last_message)
                          ELSE chats.last_message END,
      updated_at      = NOW()
  `, [chat.id, name, isGroup(chat.id), unreadCount, lastMessageTs, incomingLast, forceName]);
}

async function getChat(jid) {
  const { rows } = await pool.query('SELECT * FROM chats WHERE jid = $1', [jid]);
  return rows[0] || null;
}

async function listChats({ savedOnly = true, limit = 200, offset = 0, archived = null } = {}) {
  const params = [];
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
  if (archived !== null && archived !== undefined) {
    params.push(Boolean(archived));
    sql += ` AND c.is_archived = $${params.length}`;
  }

  params.push(limit, offset);
  sql += ` ORDER BY c.is_pinned DESC, c.last_message_ts DESC LIMIT $${params.length - 1} OFFSET $${params.length}`;

  const { rows } = await pool.query(sql, params);
  return rows;
}

async function listGroups(limit = 200, offset = 0) {
  const { rows } = await pool.query(`
    SELECT c.*, COALESCE(ct.name, '') AS contact_name, COALESCE(ct.notify, '') AS contact_notify
    FROM chats c
    LEFT JOIN contacts ct ON ct.jid = c.jid
    WHERE c.is_group = TRUE AND c.last_message_ts > 0
    ORDER BY c.last_message_ts DESC
    LIMIT $1 OFFSET $2
  `, [limit, offset]);
  return rows;
}

async function setUnreadCount(jid, count) {
  await pool.query('UPDATE chats SET unread_count = $1, updated_at = NOW() WHERE jid = $2', [Math.max(0, count), jid]);
}

async function setChatUnread(jid, count = 1) {
  await pool.query(
    'UPDATE chats SET unread_count = GREATEST(unread_count, $1), updated_at = NOW() WHERE jid = $2',
    [Math.max(1, count), jid]
  );
}

async function incrementUnread(jid) {
  await pool.query('UPDATE chats SET unread_count = unread_count + 1, updated_at = NOW() WHERE jid = $1', [jid]);
}

async function setChatArchived(jid, archived) {
  await pool.query('UPDATE chats SET is_archived = $1, updated_at = NOW() WHERE jid = $2', [Boolean(archived), jid]);
}

async function setChatPinned(jid, pinned) {
  await pool.query('UPDATE chats SET is_pinned = $1, updated_at = NOW() WHERE jid = $2', [Boolean(pinned), jid]);
}

async function setChatMuted(jid, muted, muteUntil = 0) {
  await pool.query(
    'UPDATE chats SET is_muted = $1, mute_until = $2, updated_at = NOW() WHERE jid = $3',
    [Boolean(muted), muted ? Math.max(0, Number(muteUntil) || 0) : 0, jid]
  );
}

async function recalculateLastMessage(chatJid) {
  const { rows } = await pool.query(
    'SELECT text, media_type, timestamp, deleted FROM messages WHERE chat_jid = $1 ORDER BY timestamp DESC LIMIT 1',
    [chatJid]
  );

  if (rows.length > 0) {
    const row = rows[0];
    const lastMessage = row.deleted
      ? '[Message deleted]'
      : (row.text || (row.media_type ? `[${row.media_type}]` : ''));
    await pool.query(
      'UPDATE chats SET last_message = $1, last_message_ts = $2, updated_at = NOW() WHERE jid = $3',
      [lastMessage, row.timestamp, chatJid]
    );
  } else {
    await pool.query(
      "UPDATE chats SET last_message = '', last_message_ts = 0, updated_at = NOW() WHERE jid = $1",
      [chatJid]
    );
  }
}

async function clearChatMessages(chatJid) {
  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    await client.query('DELETE FROM reactions WHERE chat_jid = $1', [chatJid]);
    await client.query('DELETE FROM messages WHERE chat_jid = $1', [chatJid]);
    // Keep the chat visible in the list with a blank preview; only deleting
    // the chat should remove the row.
    await client.query(
      "UPDATE chats SET unread_count = 0, last_message = '', updated_at = NOW() WHERE jid = $1",
      [chatJid]
    );
    await client.query('COMMIT');
  } catch (err) {
    await client.query('ROLLBACK');
    throw err;
  } finally {
    client.release();
  }
}

async function deleteChat(chatJid) {
  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    await client.query('DELETE FROM reactions WHERE chat_jid = $1', [chatJid]);
    await client.query('DELETE FROM messages WHERE chat_jid = $1', [chatJid]);
    await client.query('DELETE FROM chats WHERE jid = $1', [chatJid]);
    await client.query('COMMIT');
  } catch (err) {
    await client.query('ROLLBACK');
    throw err;
  } finally {
    client.release();
  }
}

async function getChatsUpdatedSince(since, limit = 500) {
  const { rows } = await pool.query(`
    SELECT c.*, COALESCE(ct.name, '') AS contact_name, COALESCE(ct.notify, '') AS contact_notify
    FROM chats c
    LEFT JOIN contacts ct ON ct.jid = c.jid
    WHERE c.updated_at > to_timestamp($1::double precision / 1000.0)
    ORDER BY c.updated_at ASC, c.jid ASC
    LIMIT $2
  `, [since, limit]);
  return rows;
}

// ─── Messages ────────────────────────────────────────────────────────────────

/**
 * Insert a message if it does not already exist.
 * @returns {Promise<boolean>} true only when a new row was created. This is
 * used to avoid double-counting unread messages when WhatsApp echoes a send.
 */
async function insertMessage(m) {
  const { rows } = await pool.query(`
    INSERT INTO messages
      (id, chat_jid, sender_jid, sender_name, from_me, timestamp, text,
       media_type, has_media, media_path, media_mime, media_name, media_seconds,
       is_ptt, quoted_id, status, raw_json, updated_at)
    VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,NOW())
    ON CONFLICT (chat_jid, id) DO NOTHING
    RETURNING id
  `, [
    m.id, m.chatJid, m.senderJid || '', m.senderName || '', Boolean(m.fromMe),
    m.timestamp, m.text || '', m.mediaType || null, Boolean(m.hasMedia),
    m.mediaPath || null, m.mediaMime || null, m.fileName || null,
    m.mediaSeconds || null, Boolean(m.ptt), m.quotedId || null,
    m.status || 'received', m.rawJson ? JSON.stringify(m.rawJson) : null,
  ]);
  return rows.length > 0;
}

async function updateMessageText(chatJid, msgId, text) {
  await pool.query(`
    UPDATE messages SET text = $1, edited = TRUE, updated_at = NOW()
    WHERE chat_jid = $2 AND id = $3 AND deleted = FALSE
  `, [text, chatJid, msgId]);
}

async function updateMessageContent(chatJid, msgId, parsed, { edited = false } = {}) {
  await pool.query(`
    UPDATE messages SET
      text          = $1,
      media_type    = $2,
      has_media     = $3,
      media_mime    = $4,
      media_name    = COALESCE($5, media_name),
      media_seconds = COALESCE($6, media_seconds),
      is_ptt        = COALESCE($7, is_ptt),
      edited        = CASE WHEN $8 THEN TRUE ELSE edited END,
      updated_at    = NOW()
    WHERE chat_jid = $9 AND id = $10
  `, [
    parsed.text || '', parsed.mediaType || null, Boolean(parsed.hasMedia),
    parsed.mimetype || null, parsed.fileName || null, parsed.seconds || null,
    parsed.ptt === undefined ? null : Boolean(parsed.ptt), Boolean(edited),
    chatJid, msgId,
  ]);
}

async function updateMessageMedia(chatJid, msgId, mediaPath, mediaMime, mediaName = null) {
  await pool.query(`
    UPDATE messages SET
      media_path = $1,
      media_mime = $2,
      media_name = COALESCE($3, media_name),
      updated_at = NOW()
    WHERE chat_jid = $4 AND id = $5
  `, [mediaPath, mediaMime, mediaName, chatJid, msgId]);
}

async function setMessageStatus(chatJid, msgId, status) {
  await pool.query(
    'UPDATE messages SET status = $1, updated_at = NOW() WHERE chat_jid = $2 AND id = $3',
    [status, chatJid, msgId]
  );
}

async function getMessage(chatJid, msgId) {
  const { rows } = await pool.query(
    `SELECT ${MSG_COLS} FROM messages WHERE chat_jid = $1 AND id = $2`,
    [chatJid, msgId]
  );
  return rows[0] || null;
}

async function getRawMessage(chatJid, msgId) {
  const { rows } = await pool.query(
    'SELECT raw_json FROM messages WHERE chat_jid = $1 AND id = $2 AND deleted = FALSE',
    [chatJid, msgId]
  );
  if (!rows[0]?.raw_json) return null;
  let raw = rows[0].raw_json;
  if (typeof raw === 'string') {
    try { raw = JSON.parse(raw); } catch { return null; }
  }
  return decodeRawMessage(raw);
}

async function getMessages(chatJid, { limit = 100, before = null, after = null } = {}) {
  if (before) {
    const { rows } = await pool.query(
      `SELECT ${MSG_COLS} FROM messages WHERE chat_jid = $1 AND timestamp < $2 ORDER BY timestamp DESC LIMIT $3`,
      [chatJid, before, limit]
    );
    return rows.reverse();
  }
  if (after) {
    const { rows } = await pool.query(
      `SELECT ${MSG_COLS} FROM messages WHERE chat_jid = $1 AND timestamp > $2 ORDER BY timestamp ASC LIMIT $3`,
      [chatJid, after, limit]
    );
    return rows;
  }
  const { rows } = await pool.query(
    `SELECT ${MSG_COLS} FROM messages WHERE chat_jid = $1 ORDER BY timestamp ASC LIMIT $2`,
    [chatJid, limit]
  );
  return rows;
}

async function getLatestMessages(chatJid, limit = 100) {
  const { rows } = await pool.query(`
    SELECT ${MSG_COLS} FROM (
      SELECT ${MSG_COLS} FROM messages WHERE chat_jid = $1 ORDER BY timestamp DESC LIMIT $2
    ) sub ORDER BY timestamp ASC
  `, [chatJid, limit]);
  return rows;
}

async function getMessagesUpdatedSince(since, limit = 500) {
  const { rows } = await pool.query(`
    SELECT ${MSG_COLS} FROM messages
    WHERE updated_at > to_timestamp($1::double precision / 1000.0)
    ORDER BY updated_at ASC, chat_jid ASC, id ASC
    LIMIT $2
  `, [since, limit]);
  return rows;
}

async function getMessageCount(chatJid) {
  const { rows } = await pool.query('SELECT COUNT(*)::int AS cnt FROM messages WHERE chat_jid = $1', [chatJid]);
  return rows[0]?.cnt || 0;
}

async function getReactions(chatJid, messageIds) {
  if (!messageIds || messageIds.length === 0) return [];
  const { rows } = await pool.query(`
    SELECT message_id, sender_jid, emoji, from_me, updated_at
    FROM reactions
    WHERE chat_jid = $1 AND message_id = ANY($2::text[])
  `, [chatJid, messageIds]);
  return rows;
}

async function setReaction(chatJid, messageId, senderJid, emoji, fromMe = false) {
  const target = await getMessage(chatJid, messageId);
  if (!target || target.deleted) return false;

  if (!emoji) {
    await pool.query(
      'DELETE FROM reactions WHERE chat_jid = $1 AND message_id = $2 AND sender_jid = $3',
      [chatJid, messageId, senderJid || '']
    );
  } else {
    await pool.query(`
      INSERT INTO reactions (chat_jid, message_id, sender_jid, emoji, from_me, updated_at)
      VALUES ($1, $2, $3, $4, $5, NOW())
      ON CONFLICT (chat_jid, message_id, sender_jid) DO UPDATE SET
        emoji = EXCLUDED.emoji,
        from_me = EXCLUDED.from_me,
        updated_at = NOW()
    `, [chatJid, messageId, senderJid || '', emoji, Boolean(fromMe)]);
  }

  // Reactions are part of the message payload for delta sync.
  await pool.query('UPDATE messages SET updated_at = NOW() WHERE chat_jid = $1 AND id = $2', [chatJid, messageId]);
  return true;
}

async function getQuotedMessages(chatJid, messageIds) {
  if (!messageIds || messageIds.length === 0) return [];
  const { rows } = await pool.query(`
    SELECT id, text, sender_name, from_me, media_type, deleted
    FROM messages
    WHERE chat_jid = $1 AND id = ANY($2::text[])
  `, [chatJid, messageIds]);
  return rows;
}

async function searchMessages(query, { chatJid = null, limit = 100, offset = 0 } = {}) {
  const params = [`%${query}%`];
  let sql = `
    SELECT m.id, m.chat_jid, m.sender_jid, m.sender_name, m.from_me, m.timestamp, m.text,
           m.media_type, m.has_media, m.media_path, m.media_mime, m.media_name,
           m.media_seconds, m.is_ptt, m.status, m.deleted, m.deleted_at, m.updated_at,
           m.quoted_id, m.edited,
           COALESCE(c.name, '') AS chat_name
    FROM messages m
    LEFT JOIN chats c ON c.jid = m.chat_jid
    WHERE m.deleted = FALSE AND m.text ILIKE $1
  `;
  if (chatJid) {
    params.push(chatJid);
    sql += ` AND m.chat_jid = $${params.length}`;
  }
  params.push(limit, offset);
  sql += ` ORDER BY m.timestamp DESC LIMIT $${params.length - 1} OFFSET $${params.length}`;
  const { rows } = await pool.query(sql, params);
  return rows;
}

/**
 * Delete for me: remove the row entirely.
 */
async function deleteMessage(chatJid, msgId) {
  await pool.query('DELETE FROM reactions WHERE chat_jid = $1 AND message_id = $2', [chatJid, msgId]);
  await pool.query('DELETE FROM messages WHERE chat_jid = $1 AND id = $2', [chatJid, msgId]);
  await recalculateLastMessage(chatJid);
}

/**
 * Delete for everyone: keep a tombstone so polling clients learn about the
 * deletion through /sync. Media metadata and raw protobuf are purged.
 */
async function markMessageDeleted(chatJid, msgId) {
  const { rowCount } = await pool.query(`
    UPDATE messages SET
      deleted       = TRUE,
      deleted_at    = $3,
      text          = '',
      media_type    = NULL,
      has_media     = FALSE,
      media_path    = NULL,
      media_mime    = NULL,
      media_name    = NULL,
      media_seconds = NULL,
      is_ptt        = FALSE,
      raw_json      = NULL,
      updated_at    = NOW()
    WHERE chat_jid = $1 AND id = $2 AND deleted = FALSE
  `, [chatJid, msgId, Math.floor(Date.now() / 1000)]);
  await pool.query('DELETE FROM reactions WHERE chat_jid = $1 AND message_id = $2', [chatJid, msgId]);
  if (rowCount > 0) await recalculateLastMessage(chatJid);
  return rowCount;
}

async function pruneOldMessages(chatJid, maxMessages = 1000) {
  const count = await getMessageCount(chatJid);
  if (count <= maxMessages) return [];

  const toDelete = count - maxMessages;
  const { rows } = await pool.query(
    'SELECT id FROM messages WHERE chat_jid = $1 ORDER BY timestamp ASC LIMIT $2',
    [chatJid, toDelete]
  );
  const ids = rows.map((r) => r.id);
  if (ids.length > 0) {
    await pool.query('DELETE FROM messages WHERE chat_jid = $1 AND id = ANY($2::text[])', [chatJid, ids]);
    await pool.query('DELETE FROM reactions WHERE chat_jid = $1 AND message_id = ANY($2::text[])', [chatJid, ids]);
  }
  await recalculateLastMessage(chatJid);
  return ids;
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

module.exports = {
  init, getPool, close,
  upsertContact, getContact, getAllContacts, resolveName, reResolveAllChatNames,
  upsertChat, getChat, listChats, listGroups,
  setUnreadCount, setChatUnread, incrementUnread,
  setChatArchived, setChatPinned, setChatMuted,
  recalculateLastMessage, clearChatMessages, deleteChat, getChatsUpdatedSince,
  insertMessage, updateMessageText, updateMessageContent, updateMessageMedia, setMessageStatus,
  getMessage, getRawMessage, getMessages, getLatestMessages, getMessagesUpdatedSince,
  getMessageCount, getReactions, setReaction, getQuotedMessages, searchMessages,
  deleteMessage, markMessageDeleted, pruneOldMessages,
  getStats,
};
