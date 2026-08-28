'use strict';

const pino = require('pino');
const {
  default: makeWASocket,
  DisconnectReason,
  fetchLatestBaileysVersion,
} = require('@whiskeysockets/baileys');
const { parseTimestamp, parseMessageContent, isGroup, normalizeJid } = require('./helpers');
const db = require('./db');
const media = require('./media');
const { usePostgresAuthState } = require('./auth');

let delay;
try { ({ delay } = require('@whiskeysockets/baileys')); } catch {}
if (!delay) delay = (ms) => new Promise(r => setTimeout(r, ms));

const logger = pino({ level: process.env.LOG_LEVEL || 'silent' });

let sock = null;
let latestQR = null;
let connectionStatus = 'starting';
let historySyncComplete = false;
let reconnectTimer = null;

// ─── Presence Tracking ───────────────────────────────────────────────────────
// In-memory store for online/typing status. Not persisted — ephemeral by nature.
// Key: jid (individual) or jid:participant (group)
// Value: { status, lastSeen, updatedAt }

const presenceStore = new Map();
const PRESENCE_TTL = 5 * 60 * 1000; // 5 minutes — after this, consider offline

function updatePresence(jid, participant, status) {
  const key = participant ? `${jid}:${participant}` : jid;
  const now = Date.now();

  const entry = presenceStore.get(key) || {};
  presenceStore.set(key, {
    status: status,
    lastSeen: (status === 'unavailable' && entry.status !== 'unavailable') ? now : (entry.lastSeen || null),
    updatedAt: now,
  });
}

/**
 * Get presence for a JID.
 * For individual chats: pass just jid.
 * For groups: pass group jid, returns Map of participant → presence.
 */
function getPresence(jid) {
  if (isGroup(jid)) {
    // Return all participants' presence for this group
    const prefix = jid + ':';
    const result = {};
    for (const [key, val] of presenceStore) {
      if (key.startsWith(prefix)) {
        const participant = key.substring(prefix.length);
        // Skip expired entries
        if (Date.now() - val.updatedAt > PRESENCE_TTL) continue;
        result[participant] = {
          status: val.status,
          lastSeen: val.lastSeen,
          updatedAt: val.updatedAt,
        };
      }
    }
    return result;
  }

  const entry = presenceStore.get(jid);
  if (!entry) return { status: 'unavailable', lastSeen: null, updatedAt: null };

  // Expired → treat as offline
  if (Date.now() - entry.updatedAt > PRESENCE_TTL) {
    return { status: 'unavailable', lastSeen: entry.lastSeen, updatedAt: entry.updatedAt };
  }

  return {
    status: entry.status,
    lastSeen: entry.lastSeen,
    updatedAt: entry.updatedAt,
  };
}

/**
 * Get all currently online/typing contacts.
 */
function getAllPresences() {
  const result = {};
  const now = Date.now();
  for (const [key, val] of presenceStore) {
    if (now - val.updatedAt > PRESENCE_TTL) continue;
    if (val.status === 'unavailable') continue;
    result[key] = {
      status: val.status,
      lastSeen: val.lastSeen,
      updatedAt: val.updatedAt,
    };
  }
  return result;
}

// ─── Raw Message Cache ───────────────────────────────────────────────────────

const rawMsgCache = new Map();
const RAW_CACHE_TTL = 24 * 60 * 60 * 1000;
const RAW_CACHE_MAX = 5000;

function cacheRawMsg(msg) {
  if (!msg || !msg.key) return;
  const key = `${msg.key.remoteJid}:${msg.key.id}`;
  if (rawMsgCache.size >= RAW_CACHE_MAX) {
    const cutoff = Date.now() - RAW_CACHE_TTL;
    for (const [k, v] of rawMsgCache) { if (v.ts < cutoff) rawMsgCache.delete(k); }
    if (rawMsgCache.size >= RAW_CACHE_MAX) {
      const entries = [...rawMsgCache.entries()].sort((a, b) => a[1].ts - b[1].ts);
      for (let i = 0; i < Math.floor(entries.length * 0.2); i++) rawMsgCache.delete(entries[i][0]);
    }
  }
  rawMsgCache.set(key, { msg, ts: Date.now() });
}

function getRawMsg(chatJid, msgId) {
  const key = `${chatJid}:${msgId}`;
  const entry = rawMsgCache.get(key);
  if (!entry) return null;
  if (Date.now() - entry.ts > RAW_CACHE_TTL) { rawMsgCache.delete(key); return null; }
  return entry.msg;
}

// ─── Record Logic ────────────────────────────────────────────────────────────

async function recordMessage(msg, { isHistorySync = false } = {}) {
  if (!msg || !msg.key) return;
  const chatJid = msg.key.remoteJid;
  if (!chatJid || chatJid === 'status@broadcast' || chatJid.endsWith('@newsletter')) return;

  const parsed = parseMessageContent(msg);
  if (!parsed) return;

  const fromMe = Boolean(msg.key.fromMe);
  const timestamp = parseTimestamp(msg.messageTimestamp);

  let senderJid = '';
  let senderName = '';

  if (fromMe) {
    senderJid = sock?.user?.id || '';
    senderName = 'You';
  } else if (isGroup(chatJid)) {
    senderJid = msg.key.participant || msg.participant || '';
    senderName = msg.pushName || await db.resolveName(senderJid, senderJid.replace(/@.*/, ''));
  } else {
    senderJid = chatJid;
    senderName = msg.pushName || await db.resolveName(chatJid, '');
  }

  if (msg.pushName && !fromMe) {
    const contactJid = isGroup(chatJid) ? (msg.key.participant || '') : chatJid;
    if (contactJid) await db.upsertContact({ id: contactJid, notify: msg.pushName, isSaved: false });
  }

  await db.insertMessage({
    id: msg.key.id, chatJid, senderJid, senderName, fromMe, timestamp,
    text: parsed.text, mediaType: parsed.mediaType, hasMedia: parsed.hasMedia,
    mediaPath: null, mediaMime: parsed.mimetype, status: fromMe ? 'sent' : 'received',
  });

  const existingChat = await db.getChat(chatJid);
  const chatName = existingChat?.name || await db.resolveName(chatJid, '');

  await db.upsertChat({
    id: chatJid, name: chatName,
    conversationTimestamp: timestamp,
    lastMessage: parsed.text || (parsed.mediaType ? `[${parsed.mediaType}]` : ''),
    unreadCount: 0,
  }, { isInitialSync: isHistorySync });

  if (!fromMe && !isHistorySync) await db.incrementUnread(chatJid);
  if (parsed.hasMedia) cacheRawMsg(msg);
}

async function downloadMediaBackground(msg) {
  if (!msg || !msg.key || !sock) return;
  const chatJid = msg.key.remoteJid;
  const msgId = msg.key.id;
  const existing = await db.getMessage(chatJid, msgId);
  if (existing && existing.media_path) return;

  try {
    const result = await media.downloadAndSave(msg, sock);
    if (result) await db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype);
  } catch (err) {
    logger.warn(`[Media BG] Failed ${chatJid}/${msgId}: ${err.message}`);
  }
}

// ─── WhatsApp Connection ─────────────────────────────────────────────────────

async function start() {
  // Load auth state from PostgreSQL (survives redeploys)
  const { state, saveCreds } = await usePostgresAuthState(db.getPool());
  const { version } = await fetchLatestBaileysVersion();

  // Wrap keys with cache for performance (creds stay as-is)
  const auth = {
    creds: state.creds,
    keys: state.keys,
  };

  sock = makeWASocket({
    auth, version, logger,
    printQRInTerminal: false,
    syncFullHistory: true,
    generateHighQualityLinkPreview: false,
    markOnlineOnConnect: false,
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', (update) => {
    const { connection, lastDisconnect, qr } = update;

    if (qr) { latestQR = qr; connectionStatus = 'qr'; console.log('[WA] QR issued'); }

    if (connection === 'open') {
      connectionStatus = 'connected'; latestQR = null;
      console.log(`[WA] Connected as ${sock.user?.id || 'unknown'}`);
    }

    if (connection === 'close') {
      const statusCode = lastDisconnect?.error?.output?.statusCode;
      const loggedOut = statusCode === DisconnectReason.loggedOut;
      connectionStatus = loggedOut ? 'loggedOut' : 'disconnected';
      console.log(`[WA] Closed: statusCode=${statusCode} loggedOut=${loggedOut}`);

      if (!loggedOut) {
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => start(), 5000);
      }
    }
  });

  // ── History Sync ─────────────────────────────────────────────────────────

  sock.ev.on('messaging-history.set', async ({ chats: sc, contacts: sco, messages: sm, isLatest }) => {
    console.log(`[WA] History: ${sc?.length || 0} chats, ${sco?.length || 0} contacts, ${sm?.length || 0} msgs`);

    if (sco) for (const c of sco) await db.upsertContact({ id: c.id, name: c.name || '', notify: c.notify || '', phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()) });
    if (sc) for (const c of sc) await db.upsertChat(c, { isInitialSync: true });
    if (sm && sm.length > 0) {
      const sorted = [...sm].sort((a, b) => parseTimestamp(a.messageTimestamp) - parseTimestamp(b.messageTimestamp));
      for (const msg of sorted) await recordMessage(msg, { isHistorySync: true });
    }

    await db.reResolveAllChatNames();
    if (isLatest) { historySyncComplete = true; console.log('[WA] History sync complete'); }
  });

  // ── Contact Events ───────────────────────────────────────────────────────

  sock.ev.on('contacts.upsert', async (newContacts) => {
    for (const c of (newContacts || [])) await db.upsertContact({ id: c.id, name: c.name || '', notify: c.notify || '', phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()) });
    await db.reResolveAllChatNames();
  });

  sock.ev.on('contacts.update', async (updates) => {
    for (const u of (updates || [])) { if (!u.id) continue; await db.upsertContact({ id: u.id, name: u.name || '', notify: u.notify || '', phoneNumber: u.phoneNumber || '', isSaved: Boolean(u.name?.trim()) }); }
    await db.reResolveAllChatNames();
  });

  // ── Chat Events ──────────────────────────────────────────────────────────

  sock.ev.on('chats.upsert', async (newChats) => {
    for (const c of (newChats || [])) await db.upsertChat(c, { fromEvent: true });
  });
  sock.ev.on('chats.update', async (updates) => {
    for (const u of (updates || [])) {
      if (u.id) await db.upsertChat({ id: u.id, ...u }, { fromEvent: true });
    }
  });
  sock.ev.on('chats.delete', async (deleted) => {
    for (const jid of (deleted || [])) {
      await db.getPool().query('DELETE FROM chats WHERE jid = $1', [jid]);
      await db.getPool().query('DELETE FROM messages WHERE chat_jid = $1', [jid]);
    }
  });

  // ── Message Events ───────────────────────────────────────────────────────

  sock.ev.on('messages.upsert', async ({ messages: msgs }) => {
    for (const m of (msgs || [])) {
      await recordMessage(m);
      const parsed = parseMessageContent(m);
      if (parsed && parsed.hasMedia && !m.key.fromMe) downloadMediaBackground(m);
    }
  });

  sock.ev.on('messages.update', async (updates) => {
    for (const item of (updates || [])) {
      if (!item.key) continue;
      const { remoteJid, id } = item.key;
      const upd = item.update || {};

      if (upd.status !== undefined) {
        const statusMap = { 0: 'error', 1: 'pending', 2: 'sent', 3: 'delivered', 4: 'read' };
        await db.getPool().query(
          'UPDATE messages SET status = $1 WHERE chat_jid = $2 AND id = $3',
          [statusMap[upd.status] || 'received', remoteJid, id]
        );
      }

      if (upd.message !== undefined && upd.message !== null) {
        const parsed = parseMessageContent({ message: upd.message });
        if (parsed) {
          await db.updateMessageContent(remoteJid, id, parsed.text, parsed.mediaType, parsed.hasMedia, parsed.mimetype);
          await db.recalculateLastMessage(remoteJid);
        }
      }

      if (upd.message === null && upd.status === undefined) {
        await db.deleteMessage(remoteJid, id);
      }
    }
  });

  // ── Presence Events (online / typing / recording) ────────────────────────

  sock.ev.on('presence.update', ({ id: chatJid, presences }) => {
    if (!presences) return;

    for (const [participantJid, presence] of Object.entries(presences)) {
      const status = presence.lastKnownPresence || 'unavailable';

      if (isGroup(chatJid)) {
        // Group: track per-participant presence
        updatePresence(chatJid, participantJid, status);
        logger.info(`[Presence] ${participantJid} in ${chatJid}: ${status}`);
      } else {
        // Individual chat
        updatePresence(chatJid, null, status);
        logger.info(`[Presence] ${chatJid}: ${status}`);
      }
    }
  });

  // ── Periodic Cleanup ─────────────────────────────────────────────────────

  setInterval(async () => {
    // Evict expired raw message cache entries
    const cutoff = Date.now() - RAW_CACHE_TTL;
    let n = 0;
    for (const [k, v] of rawMsgCache) { if (v.ts < cutoff) { rawMsgCache.delete(k); n++; } }
    if (n) logger.info(`[Cache] Evicted ${n} raw msg entries`);

    // Evict expired presence entries
    const presenceCutoff = Date.now() - PRESENCE_TTL;
    let pn = 0;
    for (const [k, v] of presenceStore) { if (v.updatedAt < presenceCutoff) { presenceStore.delete(k); pn++; } }
    if (pn) logger.info(`[Presence] Evicted ${pn} expired entries`);

    // Prune old messages per chat
    try {
      const { rows } = await db.getPool().query('SELECT jid FROM chats');
      let totalPruned = 0;
      for (const { jid } of rows) {
        const pruned = await db.pruneOldMessages(jid, 1000);
        totalPruned += pruned;
      }
      if (totalPruned > 0) logger.info(`[Prune] Removed ${totalPruned} old messages`);
    } catch (err) {
      logger.warn(`[Prune] Error: ${err.message}`);
    }
  }, 3600000);
}

// ─── API Functions ───────────────────────────────────────────────────────────

async function sendText(jid, text) {
  if (!sock || connectionStatus !== 'connected') throw new Error(`Not connected (${connectionStatus})`);
  jid = normalizeJid(jid);
  await sock.presenceUpdate('composing', jid);
  const result = await sock.sendMessage(jid, { text });
  await sock.presenceUpdate('paused', jid);
  if (result?.key) await recordMessage(result);
  return result;
}

async function sendMedia(jid, { buffer, mediaType, caption, mimetype, fileName }) {
  if (!sock || connectionStatus !== 'connected') throw new Error(`Not connected (${connectionStatus})`);
  jid = normalizeJid(jid);

  let payload;
  switch (mediaType) {
    case 'image': payload = { image: buffer, caption: caption || '' }; if (mimetype) payload.mimetype = mimetype; break;
    case 'video': payload = { video: buffer, caption: caption || '' }; if (mimetype) payload.mimetype = mimetype; break;
    case 'audio': payload = { audio: buffer, mimetype: mimetype || 'audio/mp4', ptt: false }; break;
    case 'sticker': payload = { sticker: buffer }; break;
    default: payload = { document: buffer, fileName: fileName || 'file', mimetype: mimetype || 'application/octet-stream' }; if (caption) payload.caption = caption; break;
  }

  const result = await sock.sendMessage(jid, payload);
  if (result?.key) await recordMessage(result);
  return result;
}

async function markChatRead(jid) {
  jid = normalizeJid(jid);
  await db.setUnreadCount(jid, 0);
  if (sock && connectionStatus === 'connected') {
    try {
      const msgs = (await db.getLatestMessages(jid, 5)).filter(m => !m.from_me).map(m => ({ key: { remoteJid: jid, id: m.id, fromMe: false }, messageTimestamp: m.timestamp }));
      if (msgs.length > 0) await sock.readMessages(msgs);
    } catch (err) { logger.warn(`[WA] Read receipt failed for ${jid}: ${err.message}`); }
  }
}

function getStatus() { return { status: connectionStatus, user: sock?.user ? { id: sock.user.id, name: sock.user.name || '' } : null, historySyncComplete }; }
function getQR() { return latestQR; }

async function downloadMediaOnDemand(chatJid, msgId) {
  const existing = media.getMediaPath(chatJid, msgId);
  if (existing) return existing;
  const rawMsg = getRawMsg(chatJid, msgId);
  if (!rawMsg) return null;
  const result = await media.downloadAndSave(rawMsg, sock);
  if (result) { await db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype); return { filePath: result.filePath, mimetype: result.mimetype }; }
  return null;
}

async function getProfilePicUrl(jid) {
  if (!sock || connectionStatus !== 'connected') return null;
  try { return await sock.profilePictureUrl(jid, 'image'); } catch { return null; }
}

async function stop() { if (reconnectTimer) clearTimeout(reconnectTimer); if (sock) { sock.end(); sock = null; } }

module.exports = {
  start, stop, sendText, sendMedia, markChatRead,
  getStatus, getQR, downloadMediaOnDemand, getProfilePicUrl,
  // Presence
  getPresence, getAllPresences,
};
