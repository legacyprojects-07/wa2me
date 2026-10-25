'use strict';

const fs = require('fs');
const path = require('path');
const pino = require('pino');
const {
  default: makeWASocket,
  DisconnectReason,
  fetchLatestBaileysVersion,
} = require('@whiskeysockets/baileys');
const {
  parseTimestamp,
  parseMessageContent,
  isGroup,
  isStatusBroadcast,
  normalizeJid,
  phoneFromJid,
  sanitizeFileName,
  encodeRawMessage,
  httpError,
  unwrapMessage,
} = require('./helpers');
const db = require('./db');
const media = require('./media');
const { useHybridAuthState } = require('./auth');

const logger = pino({ level: process.env.LOG_LEVEL || 'silent' });

const MEDIA_TYPES = new Set(['image', 'video', 'audio', 'document', 'sticker']);
const PRESENCE_STATUSES = new Set(['composing', 'paused', 'recording', 'available', 'unavailable']);

let sock = null;
let latestQR = null;
let connectionStatus = 'starting';
let historySyncComplete = false;
let reconnectTimer = null;
let cleanupTimer = null;
let manualStop = false;

// ─── Presence Tracking ───────────────────────────────────────────────────────
// Ephemeral by nature. Key: jid (DM) or jid:participant (group).

const presenceStore = new Map();
const PRESENCE_TTL = 5 * 60 * 1000;

function updatePresence(jid, participant, status) {
  const key = participant ? `${jid}:${participant}` : jid;
  const now = Date.now();
  const entry = presenceStore.get(key) || {};
  presenceStore.set(key, {
    status,
    lastSeen: (status === 'unavailable' && entry.status !== 'unavailable') ? now : (entry.lastSeen || null),
    updatedAt: now,
  });
}

function getPresence(jid) {
  if (isGroup(jid)) {
    const prefix = jid + ':';
    const result = {};
    for (const [key, val] of presenceStore) {
      if (!key.startsWith(prefix)) continue;
      if (Date.now() - val.updatedAt > PRESENCE_TTL) continue;
      result[key.substring(prefix.length)] = {
        status: val.status,
        lastSeen: val.lastSeen,
        updatedAt: val.updatedAt,
      };
    }
    return result;
  }

  const entry = presenceStore.get(jid);
  if (!entry) return { status: 'unavailable', lastSeen: null, updatedAt: null };
  if (Date.now() - entry.updatedAt > PRESENCE_TTL) {
    return { status: 'unavailable', lastSeen: entry.lastSeen, updatedAt: entry.updatedAt };
  }
  return { status: entry.status, lastSeen: entry.lastSeen, updatedAt: entry.updatedAt };
}

function getAllPresences() {
  const result = {};
  const now = Date.now();
  for (const [key, val] of presenceStore) {
    if (now - val.updatedAt > PRESENCE_TTL) continue;
    if (val.status === 'unavailable') continue;
    result[key] = { status: val.status, lastSeen: val.lastSeen, updatedAt: val.updatedAt };
  }
  return result;
}

// ─── Raw Message Cache ───────────────────────────────────────────────────────
// Speed cache only. The durable copy lives in messages.raw_json.

const rawMsgCache = new Map();
const RAW_CACHE_TTL = 24 * 60 * 60 * 1000;
const RAW_CACHE_MAX = 2000;

function cacheRawMsg(msg) {
  if (!msg || !msg.key) return;
  const key = `${msg.key.remoteJid}:${msg.key.id}`;
  if (rawMsgCache.size >= RAW_CACHE_MAX) {
    const entries = [...rawMsgCache.entries()].sort((a, b) => a[1].ts - b[1].ts);
    for (let i = 0; i < Math.floor(entries.length * 0.2); i++) rawMsgCache.delete(entries[i][0]);
  }
  rawMsgCache.set(key, { msg, ts: Date.now() });
}

function getCachedRawMsg(chatJid, msgId) {
  const key = `${chatJid}:${msgId}`;
  const entry = rawMsgCache.get(key);
  if (!entry) return null;
  if (Date.now() - entry.ts > RAW_CACHE_TTL) { rawMsgCache.delete(key); return null; }
  return entry.msg;
}

async function getRawMsg(chatJid, msgId) {
  return getCachedRawMsg(chatJid, msgId) || await db.getRawMessage(chatJid, msgId);
}

// ─── Record Logic ────────────────────────────────────────────────────────────

async function recordMessage(msg, { isHistorySync = false } = {}) {
  if (!msg || !msg.key || !msg.key.id) return false;
  const chatJid = msg.key.remoteJid;
  if (isStatusBroadcast(chatJid)) return false;

  const content = unwrapMessage(msg.message);
  if (content?.protocolMessage || content?.reactionMessage) return false;

  const parsed = parseMessageContent(msg);
  if (!parsed) return false;

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
    if (contactJid) {
      await db.upsertContact({ id: contactJid, notify: msg.pushName, isSaved: false });
    }
  }

  const rawJson = parsed.storeRaw
    ? encodeRawMessage({
        key: msg.key,
        message: msg.message,
        messageTimestamp: msg.messageTimestamp,
        pushName: msg.pushName,
      })
    : null;

  const inserted = await db.insertMessage({
    id: msg.key.id,
    chatJid,
    senderJid,
    senderName,
    fromMe,
    timestamp,
    text: parsed.text,
    mediaType: parsed.mediaType,
    hasMedia: parsed.hasMedia,
    mediaMime: parsed.mimetype,
    fileName: parsed.fileName,
    mediaSeconds: parsed.seconds,
    ptt: parsed.ptt,
    quotedId: parsed.quotedId,
    status: fromMe ? 'sent' : 'received',
    rawJson,
  });

  const existingChat = await db.getChat(chatJid);
  const chatName = existingChat?.name || await db.resolveName(chatJid, '');

  await db.upsertChat({
    id: chatJid,
    name: chatName,
    conversationTimestamp: timestamp,
    lastMessage: parsed.text || (parsed.mediaType ? `[${parsed.mediaType}]` : ''),
    unreadCount: 0,
  }, { isInitialSync: isHistorySync, preserveUnread: isHistorySync });

  // Only increment for a genuinely new incoming row. WhatsApp echoes our own
  // sends and history sync replays old messages; neither must bump the badge.
  if (inserted && !fromMe && !isHistorySync) await db.incrementUnread(chatJid);
  if (parsed.storeRaw) cacheRawMsg(msg);
  return inserted;
}

const inFlightDownloads = new Set();
const mediaDownloadQueue = [];
const queuedMediaDownloads = new Set();
let activeMediaDownloads = 0;
const MAX_CONCURRENT_DOWNLOADS = 2;

/** Queue background media downloads with bounded concurrency. */
function scheduleMediaDownload(msg) {
  if (!msg?.key?.remoteJid || !msg.key.id) return;
  const key = `${msg.key.remoteJid}:${msg.key.id}`;
  if (inFlightDownloads.has(key) || queuedMediaDownloads.has(key)) return;
  queuedMediaDownloads.add(key);
  mediaDownloadQueue.push({ key, msg });
  drainMediaDownloads();
}

function drainMediaDownloads() {
  while (activeMediaDownloads < MAX_CONCURRENT_DOWNLOADS && mediaDownloadQueue.length > 0) {
    const item = mediaDownloadQueue.shift();
    queuedMediaDownloads.delete(item.key);
    activeMediaDownloads++;
    downloadMediaBackground(item.msg)
      .catch((err) => logger.warn(`[Media BG] ${item.key}: ${err.message}`))
      .finally(() => {
        activeMediaDownloads--;
        drainMediaDownloads();
      });
  }
}

async function downloadMediaBackground(msg) {
  if (!msg || !msg.key || !sock) return;
  const chatJid = msg.key.remoteJid;
  const msgId = msg.key.id;
  const cacheKey = `${chatJid}:${msgId}`;
  if (!chatJid || !msgId || inFlightDownloads.has(cacheKey)) return;

  try {
    const existing = await db.getMessage(chatJid, msgId);
    if (!existing || existing.deleted || !existing.has_media) return;
    const already = await media.getMediaPath(chatJid, msgId);
    if (already?.filePath && !already.isThumbnail) return;

    inFlightDownloads.add(cacheKey);
    const result = await media.downloadAndSave(msg, sock);
    if (result) {
      await db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype, result.fileName);
    }
  } catch (err) {
    logger.warn(`[Media BG] Failed ${chatJid}/${msgId}: ${err.message}`);
  } finally {
    inFlightDownloads.delete(cacheKey);
  }
}

// ─── WhatsApp Connection ─────────────────────────────────────────────────────

function safeHandler(name, fn) {
  return (...args) => {
    Promise.resolve()
      .then(() => fn(...args))
      .catch((err) => logger.error(`[WA Event] ${name}: ${err.message}`));
  };
}

async function start() {
  if (connectionStatus === 'connected' || connectionStatus === 'qr') {
    logger.info('[WA] start() ignored — already running/connecting');
    return;
  }

  manualStop = false;
  historySyncComplete = false;
  connectionStatus = 'starting';
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  if (cleanupTimer) { clearInterval(cleanupTimer); cleanupTimer = null; }

  const authDir = path.resolve(process.env.AUTH_DIR || './auth_state');
  const { state, saveCreds } = await useHybridAuthState(authDir, db.getPool());
  const { version } = await fetchLatestBaileysVersion();

  sock = makeWASocket({
    auth: { creds: state.creds, keys: state.keys },
    version,
    logger,
    printQRInTerminal: false,
    syncFullHistory: true,
    generateHighQualityLinkPreview: false,
    markOnlineOnConnect: true,
  });
  const socketRef = sock;

  sock.ev.on('creds.update', () => {
    Promise.resolve()
      .then(() => saveCreds())
      .catch((err) => console.error('[Auth] saveCreds failed:', err.message));
  });

  sock.ev.on('connection.update', (update) => {
    // Ignore a stale socket that is closing after /reset or a reconnect.
    if (sock !== socketRef) return;

    const { connection, lastDisconnect, qr } = update;

    if (qr) {
      latestQR = qr;
      connectionStatus = 'qr';
      console.log('[WA] QR issued');
    }

    if (connection === 'connecting' && connectionStatus !== 'qr') {
      connectionStatus = 'starting';
    }

    if (connection === 'open') {
      connectionStatus = 'connected';
      latestQR = null;
      console.log(`[WA] Connected as ${socketRef.user?.id || 'unknown'}`);
    }

    if (connection === 'close') {
      const statusCode = lastDisconnect?.error?.output?.statusCode;
      const loggedOut = statusCode === DisconnectReason.loggedOut;
      connectionStatus = loggedOut ? 'loggedOut' : (manualStop ? 'stopped' : 'disconnected');
      latestQR = null;
      console.log(`[WA] Closed: statusCode=${statusCode} loggedOut=${loggedOut}`);

      if (!loggedOut && !manualStop) {
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => {
          start().catch((err) => console.error('[WA] Reconnect failed:', err.message));
        }, 5000);
      }
    }
  });

  // ── History Sync ─────────────────────────────────────────────────────────

  sock.ev.on('messaging-history.set', safeHandler('messaging-history.set', async ({ chats: sc, contacts: sco, messages: sm, isLatest }) => {
    console.log(`[WA] History: ${sc?.length || 0} chats, ${sco?.length || 0} contacts, ${sm?.length || 0} msgs`);

    if (sco) {
      for (const c of sco) {
        await db.upsertContact({
          id: c.id, name: c.name || '', notify: c.notify || '',
          phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()),
        });
      }
    }
    if (sc) {
      for (const c of sc) await db.upsertChat(c, { isInitialSync: true });
    }
    if (sm && sm.length > 0) {
      const sorted = [...sm].sort((a, b) => parseTimestamp(a.messageTimestamp) - parseTimestamp(b.messageTimestamp));
      for (const msg of sorted) await recordMessage(msg, { isHistorySync: true });
    }

    await db.reResolveAllChatNames();
    if (isLatest) {
      historySyncComplete = true;
      console.log('[WA] History sync complete');
    }
  }));

  // ── Contacts ─────────────────────────────────────────────────────────────

  sock.ev.on('contacts.upsert', safeHandler('contacts.upsert', async (newContacts) => {
    for (const c of (newContacts || [])) {
      await db.upsertContact({
        id: c.id, name: c.name || '', notify: c.notify || '',
        phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()),
      });
    }
    await db.reResolveAllChatNames();
  }));

  sock.ev.on('contacts.update', safeHandler('contacts.update', async (updates) => {
    for (const u of (updates || [])) {
      if (!u.id) continue;
      await db.upsertContact({
        id: u.id, name: u.name || '', notify: u.notify || '',
        phoneNumber: u.phoneNumber || '', isSaved: Boolean(u.name?.trim()),
      });
    }
    await db.reResolveAllChatNames();
  }));

  // ── Chats ────────────────────────────────────────────────────────────────

  sock.ev.on('chats.upsert', safeHandler('chats.upsert', async (newChats) => {
    for (const c of (newChats || [])) await db.upsertChat(c, { fromEvent: true });
  }));

  sock.ev.on('chats.update', safeHandler('chats.update', async (updates) => {
    for (const u of (updates || [])) {
      if (!u.id) continue;
      if (u.archived !== undefined && u.archived !== null) await db.setChatArchived(u.id, u.archived);
      if (u.pinned !== undefined && u.pinned !== null) await db.setChatPinned(u.id, u.pinned);
      if (u.muted !== undefined) {
        const until = u.muteEndTime || u.muteEndTimestamp || 0;
        await db.setChatMuted(u.id, Boolean(u.muted), until);
      }
      await db.upsertChat({ id: u.id, ...u }, { fromEvent: true });
    }
  }));

  sock.ev.on('chats.delete', safeHandler('chats.delete', async (deleted) => {
    for (const jid of (deleted || [])) {
      media.deleteChatMediaFiles(jid);
      await db.deleteChat(jid);
    }
  }));

  // ── Groups ───────────────────────────────────────────────────────────────

  const refreshGroup = async (g) => {
    if (!g || !g.id) return;
    await db.upsertChat({ id: g.id, name: g.subject || '', isGroup: true }, { forceName: Boolean(g.subject), fromEvent: true });
  };
  sock.ev.on('groups.upsert', safeHandler('groups.upsert', async (groups) => {
    for (const g of (groups || [])) await refreshGroup(g);
  }));
  sock.ev.on('groups.update', safeHandler('groups.update', async (groups) => {
    for (const g of (groups || [])) await refreshGroup(g);
  }));

  // ── Messages ─────────────────────────────────────────────────────────────

  sock.ev.on('messages.upsert', safeHandler('messages.upsert', async ({ messages: msgs }) => {
    for (const m of (msgs || [])) {
      await recordMessage(m);
      const parsed = parseMessageContent(m);
      if (parsed?.hasMedia) scheduleMediaDownload(m);
    }
  }));

  sock.ev.on('messages.reaction', safeHandler('messages.reaction', async (items) => {
    for (const item of (items || [])) {
      const target = item.key;
      const reaction = item.reaction || {};
      if (!target || !target.id) continue;

      const chatJid = target.remoteJid || reaction.key?.remoteJid;
      if (!chatJid) continue;

      const fromMe = Boolean(reaction.key?.fromMe);
      const senderJid = fromMe
        ? (sock?.user?.id || '')
        : (isGroup(chatJid)
            ? (reaction.key?.participant || reaction.key?.remoteJid || '')
            : (reaction.key?.remoteJid || chatJid));

      await db.setReaction(chatJid, target.id, senderJid, reaction.text || '', fromMe);
    }
  }));

  sock.ev.on('messages.delete', safeHandler('messages.delete', async (event) => {
    if (!event) return;
    if (event.all && event.jid) {
      media.deleteChatMediaFiles(event.jid);
      await db.clearChatMessages(event.jid);
      return;
    }
    for (const key of (event.keys || [])) {
      if (!key?.remoteJid || !key.id) continue;
      media.deleteMediaFiles(key.remoteJid, key.id);
      await db.deleteMessage(key.remoteJid, key.id);
    }
  }));

  sock.ev.on('messages.update', safeHandler('messages.update', async (updates) => {
    for (const item of (updates || [])) {
      if (!item.key) continue;
      const { remoteJid, id } = item.key;
      if (!remoteJid || !id) continue;
      const upd = item.update || {};

      if (upd.status !== undefined && upd.status !== null) {
        const statusMap = {
          0: 'error', 1: 'pending', 2: 'sent', 3: 'delivered', 4: 'read', 5: 'played',
        };
        await db.setMessageStatus(remoteJid, id, statusMap[upd.status] || 'received');
      }

      if (upd.message === null) {
        // Revokes arrive as message:null. Keep a tombstone so /sync clients
        // remove the bubble instead of silently keeping stale content.
        media.deleteMediaFiles(remoteJid, id);
        await db.markMessageDeleted(remoteJid, id);
        continue;
      }

      if (upd.message !== undefined && upd.message !== null) {
        const parsed = parseMessageContent({ message: upd.message });
        if (parsed) {
          const isEdit = Boolean(upd.message.editedMessage);
          await db.updateMessageContent(remoteJid, id, parsed, { edited: isEdit });
          await db.recalculateLastMessage(remoteJid);
        }
      }
    }
  }));

  // ── Presence (online / typing / recording) ───────────────────────────────

  sock.ev.on('presence.update', ({ id: chatJid, presences }) => {
    if (!presences) return;
    for (const [participantJid, presence] of Object.entries(presences)) {
      const status = presence.lastKnownPresence || 'unavailable';
      if (isGroup(chatJid)) {
        updatePresence(chatJid, participantJid, status);
      } else {
        updatePresence(chatJid, null, status);
      }
    }
  });

  // ── Periodic Cleanup ─────────────────────────────────────────────────────

  cleanupTimer = setInterval(async () => {
    const cutoff = Date.now() - RAW_CACHE_TTL;
    for (const [k, v] of rawMsgCache) { if (v.ts < cutoff) rawMsgCache.delete(k); }

    const presenceCutoff = Date.now() - PRESENCE_TTL;
    for (const [k, v] of presenceStore) { if (v.updatedAt < presenceCutoff) presenceStore.delete(k); }

    try {
      const { rows } = await db.getPool().query('SELECT jid FROM chats');
      let totalPruned = 0;
      for (const { jid } of rows) {
        const removedIds = await db.pruneOldMessages(jid, 1000);
        for (const id of removedIds) media.deleteMediaFiles(jid, id);
        totalPruned += removedIds.length;
      }
      if (totalPruned > 0) logger.info(`[Prune] Removed ${totalPruned} old messages`);
    } catch (err) {
      logger.warn(`[Prune] Error: ${err.message}`);
    }
  }, 3600000);
  cleanupTimer.unref?.();
}

// ─── Socket helpers ──────────────────────────────────────────────────────────

function ensureConnected() {
  if (!sock || connectionStatus !== 'connected') {
    throw httpError(503, `WhatsApp is not connected (${connectionStatus})`, 'NOT_CONNECTED');
  }
  return sock;
}

function isConnected() {
  return Boolean(sock && connectionStatus === 'connected');
}

function ownJid() {
  return sock?.user?.id || '';
}

function buildMessageKey(jid, row) {
  const key = { remoteJid: jid, fromMe: Boolean(row.from_me), id: row.id };
  if (isGroup(jid)) key.participant = row.from_me ? ownJid() : (row.sender_jid || undefined);
  return key;
}

async function buildQuoted(jid, quotedId) {
  if (!quotedId) return null;

  const raw = await getRawMsg(jid, quotedId);
  if (raw?.message) {
    return {
      key: raw.key || { remoteJid: jid, id: quotedId },
      message: raw.message,
    };
  }

  const row = await db.getMessage(jid, quotedId);
  if (!row || row.deleted) throw httpError(404, 'Quoted message not found', 'QUOTED_NOT_FOUND');

  const map = {
    image: 'imageMessage', video: 'videoMessage', audio: 'audioMessage',
    document: 'documentMessage', sticker: 'stickerMessage',
  };
  let message;
  if (row.media_type && map[row.media_type]) {
    message = {
      [map[row.media_type]]: {
        caption: row.text || undefined,
        mimetype: row.media_mime || undefined,
        fileName: row.media_name || undefined,
      },
    };
  } else {
    message = { conversation: row.text || '' };
  }

  return { key: buildMessageKey(jid, row), message };
}

async function sendWithPresence(jid, status, fn) {
  try { await sock.sendPresenceUpdate(status, jid); } catch {}
  try {
    return await fn();
  } finally {
    try { await sock.sendPresenceUpdate('paused', jid); } catch {}
  }
}

// ─── Sending ─────────────────────────────────────────────────────────────────

async function sendText(jid, text, { quotedId } = {}) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!text || typeof text !== 'string' || !text.trim()) {
    throw httpError(400, 'text is required', 'TEXT_REQUIRED');
  }
  if (text.length > 65536) throw httpError(400, 'text is too long (max 65536 characters)', 'TEXT_TOO_LONG');

  const quoted = await buildQuoted(jid, quotedId);
  const payload = { text };
  if (quoted) payload.quoted = quoted;

  let result;
  try {
    result = await sendWithPresence(jid, 'composing', () => sock.sendMessage(jid, payload));
  } catch (err) {
    throw httpError(502, `WhatsApp send failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
  }
  if (result?.key) await recordMessage(result);
  return result;
}

async function sendMedia(jid, { buffer, mediaType, caption, mimetype, fileName, ptt, quotedId, seconds }) {
  ensureConnected();
  jid = normalizeJid(jid);

  if (!buffer || !Buffer.isBuffer(buffer) || buffer.length === 0) {
    throw httpError(400, 'A non-empty file is required', 'FILE_REQUIRED');
  }
  if (!MEDIA_TYPES.has(mediaType)) {
    throw httpError(400, `Invalid mediaType. Use image, video, audio, document or sticker`, 'INVALID_MEDIA_TYPE');
  }
  if (ptt && mediaType !== 'audio') {
    throw httpError(400, 'ptt=1 is only valid for audio', 'INVALID_PTT');
  }

  const quoted = await buildQuoted(jid, quotedId);
  const defaultNames = {
    image: 'photo.jpg', video: 'video.mp4',
    audio: ptt ? 'voice.ogg' : 'audio.mp4',
    document: 'file', sticker: 'sticker.webp',
  };
  const safeName = sanitizeFileName(fileName || defaultNames[mediaType] || 'file');

  const defaults = {
    image: 'image/jpeg', video: 'video/mp4',
    audio: ptt ? 'audio/ogg; codecs=opus' : 'audio/mp4',
    document: 'application/octet-stream', sticker: 'image/webp',
  };
  let mime = mimetype || defaults[mediaType];
  if (!mime || mime === 'application/octet-stream' || mime === 'binary/octet-stream') {
    mime = defaults[mediaType];
  }

  let payload;
  switch (mediaType) {
    case 'image':
      payload = { image: buffer, caption: caption || '', mimetype: mime };
      break;
    case 'video':
      payload = { video: buffer, caption: caption || '', mimetype: mime };
      if (Number(seconds) > 0) payload.seconds = Number(seconds);
      break;
    case 'audio':
      payload = { audio: buffer, mimetype: mime, ptt: Boolean(ptt) };
      if (Number(seconds) > 0) payload.seconds = Number(seconds);
      break;
    case 'sticker':
      payload = { sticker: buffer };
      break;
    default:
      payload = { document: buffer, fileName: safeName, mimetype: mime };
      if (caption) payload.caption = caption;
      break;
  }
  if (quoted) payload.quoted = quoted;

  const presence = ptt ? 'recording' : 'composing';
  let result;
  try {
    result = await sendWithPresence(jid, presence, () => sock.sendMessage(jid, payload));
  } catch (err) {
    throw httpError(502, `WhatsApp media send failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
  }

  if (result?.key) {
    await recordMessage(result);
    try {
      const saved = await media.saveOutgoing(jid, result.key.id, buffer, {
        mediaType, mimetype: mime, fileName: safeName,
      });
      if (saved) {
        await db.updateMessageMedia(jid, result.key.id, saved.filePath, saved.mimetype, safeName);
      }
    } catch (err) {
      logger.warn(`[Send Media] Could not cache outgoing file: ${err.message}`);
    }
  }
  return result;
}

async function sendContact(jid, { name, phone, quotedId }) {
  ensureConnected();
  jid = normalizeJid(jid);
  const displayName = String(name || '').trim();
  if (!displayName) throw httpError(400, 'name is required', 'NAME_REQUIRED');

  const digits = String(phone || '').replace(/[^\d]/g, '');
  if (!/^\d{5,20}$/.test(digits)) throw httpError(400, 'A valid phone number is required', 'PHONE_REQUIRED');

  const vcard = [
    'BEGIN:VCARD',
    'VERSION:3.0',
    `FN:${displayName.replace(/[\r\n;,]/g, ' ')}`,
    `TEL;type=CELL;type=VOICE;waid=${digits}:+${digits}`,
    'END:VCARD',
  ].join('\r\n');

  const quoted = await buildQuoted(jid, quotedId);
  const payload = {
    contacts: {
      displayName,
      contacts: [{ vcard }],
    },
  };
  if (quoted) payload.quoted = quoted;

  let result;
  try {
    result = await sendWithPresence(jid, 'composing', () => sock.sendMessage(jid, payload));
  } catch (err) {
    throw httpError(502, `WhatsApp contact send failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
  }
  if (result?.key) await recordMessage(result);
  return result;
}

async function sendLocation(jid, { latitude, longitude, name, address, quotedId }) {
  ensureConnected();
  jid = normalizeJid(jid);
  const lat = Number(latitude);
  const lng = Number(longitude);
  if (!Number.isFinite(lat) || !Number.isFinite(lng) || lat < -90 || lat > 90 || lng < -180 || lng > 180) {
    throw httpError(400, 'Valid latitude and longitude are required', 'INVALID_LOCATION');
  }

  const quoted = await buildQuoted(jid, quotedId);
  const payload = {
    location: {
      degreesLatitude: lat,
      degreesLongitude: lng,
      name: name || undefined,
      address: address || undefined,
    },
  };
  if (quoted) payload.quoted = quoted;

  let result;
  try {
    result = await sendWithPresence(jid, 'composing', () => sock.sendMessage(jid, payload));
  } catch (err) {
    throw httpError(502, `WhatsApp location send failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
  }
  if (result?.key) await recordMessage(result);
  return result;
}

// ─── Message actions ─────────────────────────────────────────────────────────

async function deleteForEveryone(jid, msgId) {
  ensureConnected();
  jid = normalizeJid(jid);
  const row = await db.getMessage(jid, msgId);
  if (!row) throw httpError(404, 'Message not found', 'MESSAGE_NOT_FOUND');
  if (!row.from_me) throw httpError(403, 'You can only delete your own messages for everyone', 'NOT_YOUR_MESSAGE');
  if (row.deleted) return { alreadyDeleted: true };

  const key = buildMessageKey(jid, row);
  try {
    await sock.sendMessage(jid, { delete: key });
  } catch (err) {
    throw httpError(409, `WhatsApp refused the delete (the message may be too old): ${err.message}`, 'REVOKE_FAILED');
  }

  media.deleteMediaFiles(jid, msgId);
  await db.markMessageDeleted(jid, msgId);
  return { alreadyDeleted: false };
}

async function deleteForMe(jid, msgId) {
  jid = normalizeJid(jid);
  const row = await db.getMessage(jid, msgId);
  if (!row) throw httpError(404, 'Message not found', 'MESSAGE_NOT_FOUND');
  media.deleteMediaFiles(jid, msgId);
  await db.deleteMessage(jid, msgId);
  return { deleted: true };
}

async function editMessage(jid, msgId, text) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!text || typeof text !== 'string' || !text.trim()) {
    throw httpError(400, 'text is required', 'TEXT_REQUIRED');
  }
  const row = await db.getMessage(jid, msgId);
  if (!row) throw httpError(404, 'Message not found', 'MESSAGE_NOT_FOUND');
  if (!row.from_me) throw httpError(403, 'You can only edit your own messages', 'NOT_YOUR_MESSAGE');
  if (row.deleted) throw httpError(410, 'Message was deleted', 'MESSAGE_DELETED');
  if (row.media_type) throw httpError(400, 'Only text messages can be edited', 'NOT_EDITABLE');

  const key = buildMessageKey(jid, row);
  try {
    await sock.sendMessage(jid, { text, edit: key });
  } catch (err) {
    throw httpError(409, `WhatsApp refused the edit (it may be too old): ${err.message}`, 'EDIT_FAILED');
  }
  await db.updateMessageText(jid, msgId, text);
  await db.recalculateLastMessage(jid);
  return { text };
}

async function sendReaction(jid, msgId, emoji) {
  ensureConnected();
  jid = normalizeJid(jid);
  emoji = String(emoji ?? '');
  if (Array.from(emoji).length > 8) throw httpError(400, 'Reaction is too long', 'REACTION_TOO_LONG');

  const row = await db.getMessage(jid, msgId);
  if (!row) throw httpError(404, 'Message not found', 'MESSAGE_NOT_FOUND');
  if (row.deleted) throw httpError(410, 'Message was deleted', 'MESSAGE_DELETED');

  const key = buildMessageKey(jid, row);
  try {
    await sock.sendMessage(jid, { react: { text: emoji, key } });
  } catch (err) {
    throw httpError(502, `WhatsApp reaction failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
  }
  await db.setReaction(jid, msgId, ownJid(), emoji, true);
  return { emoji, removed: !emoji };
}

async function forwardMessage(fromJid, msgId, toJid) {
  ensureConnected();
  fromJid = normalizeJid(fromJid);
  toJid = normalizeJid(toJid);

  const row = await db.getMessage(fromJid, msgId);
  if (!row) throw httpError(404, 'Message not found', 'MESSAGE_NOT_FOUND');
  if (row.deleted) throw httpError(410, 'Message was deleted', 'MESSAGE_DELETED');
  if (fromJid === toJid) throw httpError(400, 'Cannot forward to the same chat', 'SAME_CHAT');

  const raw = await getRawMsg(fromJid, msgId);

  // Media: re-upload our cached copy so the new message is self-contained and
  // /sync/media works immediately on the target device.
  if (row.has_media) {
    let info = await media.getMediaPath(fromJid, msgId);
    if ((!info || !info.filePath) && raw) {
      try {
        info = await downloadMediaOnDemand(fromJid, msgId);
      } catch {}
    }
    if (info?.filePath && !info.isThumbnail) {
      const buffer = await fs.promises.readFile(info.filePath);
      return sendMedia(toJid, {
        buffer,
        mediaType: row.media_type,
        caption: row.text || '',
        mimetype: info.mimetype || row.media_mime,
        fileName: row.media_name,
        ptt: Boolean(row.is_ptt),
      });
    }
  }

  // Exact native forward preserves location/contact/video payloads when the
  // original protobuf is available (including after a restart, from DB).
  if (raw?.message) {
    let result;
    try {
      result = await sock.sendMessage(toJid, { forward: raw, force: true });
    } catch (err) {
      throw httpError(502, `WhatsApp forward failed: ${err.message}`, 'WHATSAPP_SEND_FAILED');
    }
    if (result?.key) await recordMessage(result);
    return result;
  }

  if (row.text) return sendText(toJid, row.text);
  throw httpError(409, 'This message can no longer be forwarded (media expired)', 'NOT_FORWARDABLE');
}

// ─── Chat actions ────────────────────────────────────────────────────────────

async function markChatRead(jid) {
  jid = normalizeJid(jid);
  await db.setUnreadCount(jid, 0);
  if (!isConnected()) return;

  try {
    const rows = (await db.getLatestMessages(jid, 10)).filter((m) => !m.from_me && !m.deleted);
    if (rows.length === 0) return;
    const keys = rows.map((m) => {
      const key = { remoteJid: jid, id: m.id, fromMe: false };
      if (isGroup(jid) && m.sender_jid) key.participant = m.sender_jid;
      return key;
    });
    await sock.readMessages(keys);
  } catch (err) {
    logger.warn(`[WA] Read receipt failed for ${jid}: ${err.message}`);
  }
}

async function markChatUnread(jid, count = 1) {
  jid = normalizeJid(jid);
  await db.setChatUnread(jid, count);
  const warnings = [];
  if (isConnected()) {
    try {
      const last = (await db.getLatestMessages(jid, 3)).map((m) => ({
        key: buildMessageKey(jid, m),
        messageTimestamp: Number(m.timestamp),
      }));
      await sock.chatModify({ markRead: false, lastMessages: last }, jid);
    } catch (err) {
      warnings.push(err.message);
    }
  }
  return { warnings };
}

async function setChatArchived(jid, archived) {
  jid = normalizeJid(jid);
  await db.setChatArchived(jid, archived);
  const warnings = [];
  if (isConnected()) {
    try { await sock.chatModify({ archive: Boolean(archived) }, jid); }
    catch (err) { warnings.push(err.message); }
  }
  return { archived: Boolean(archived), warnings };
}

async function setChatPinned(jid, pinned) {
  jid = normalizeJid(jid);
  await db.setChatPinned(jid, pinned);
  const warnings = [];
  if (isConnected()) {
    try { await sock.chatModify({ pin: Boolean(pinned) }, jid); }
    catch (err) { warnings.push(err.message); }
  }
  return { pinned: Boolean(pinned), warnings };
}

async function setChatMuted(jid, muted, durationSeconds = 8 * 60 * 60) {
  jid = normalizeJid(jid);
  const until = muted ? Math.floor(Date.now() / 1000) + Math.max(60, Number(durationSeconds) || 28800) : 0;
  await db.setChatMuted(jid, muted, until);
  const warnings = [];
  if (isConnected()) {
    try { await sock.chatModify({ mute: muted ? until : null }, jid); }
    catch (err) { warnings.push(err.message); }
  }
  return { muted: Boolean(muted), muteUntil: until, warnings };
}

async function clearChat(jid, { remote = true } = {}) {
  jid = normalizeJid(jid);
  const last = (await db.getLatestMessages(jid, 3)).map((m) => ({
    key: buildMessageKey(jid, m),
    messageTimestamp: Number(m.timestamp),
  }));

  media.deleteChatMediaFiles(jid);
  await db.clearChatMessages(jid);

  const warnings = [];
  if (remote && isConnected()) {
    try {
      await sock.chatModify({ clear: true, lastMessages: last }, jid);
    } catch (err) {
      warnings.push(err.message);
    }
  }
  return { cleared: true, warnings };
}

async function deleteChat(jid, { remote = true } = {}) {
  jid = normalizeJid(jid);
  const last = (await db.getLatestMessages(jid, 3)).map((m) => ({
    key: buildMessageKey(jid, m),
    messageTimestamp: Number(m.timestamp),
  }));

  media.deleteChatMediaFiles(jid);
  await db.deleteChat(jid);

  const warnings = [];
  if (remote && isConnected()) {
    try {
      await sock.chatModify({ delete: true, lastMessages: last }, jid);
    } catch (err) {
      warnings.push(err.message);
    }
  }
  return { deleted: true, warnings };
}

async function sendPresenceUpdate(jid, status) {
  ensureConnected();
  if (!PRESENCE_STATUSES.has(status)) {
    throw httpError(400, 'Invalid presence status', 'INVALID_PRESENCE');
  }
  jid = normalizeJid(jid);
  await sock.sendPresenceUpdate(status, jid);
}

/** Subscribe to a contact's presence so WhatsApp sends us updates. */
async function subscribePresence(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  await sock.presenceSubscribe(jid);
  return { subscribed: true };
}

/** Set our own global presence (available/unavailable). */
async function setOwnPresence(status) {
  ensureConnected();
  if (!['available', 'unavailable'].includes(status)) {
    throw httpError(400, 'status must be available or unavailable', 'INVALID_PRESENCE');
  }
  await sock.sendPresenceUpdate(status);
  return { status };
}

// ─── Profile / contacts ──────────────────────────────────────────────────────

function getMe() {
  if (!sock?.user) return null;
  return {
    id: sock.user.id,
    name: sock.user.name || '',
    phoneNumber: phoneFromJid(sock.user.id),
    lid: sock.user.lid || null,
  };
}

async function getAbout(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  try {
    const list = await sock.fetchStatus(jid);
    const item = (list || []).find((x) => x.id === jid) || (list || [])[0];
    if (!item) return { status: null, setAt: null };
    const raw = item.status;
    if (!raw) return { status: null, setAt: null };
    const text = typeof raw === 'string' ? raw : (raw.status ?? null);
    const setAtRaw = typeof raw === 'object' ? raw.setAt : null;
    const setAt = setAtRaw instanceof Date ? Math.floor(setAtRaw.getTime() / 1000) : (setAtRaw ? parseTimestamp(setAtRaw) : null);
    return { status: text, setAt };
  } catch {
    return { status: null, setAt: null };
  }
}

async function updateProfileStatus(status) {
  ensureConnected();
  if (typeof status !== 'string' || status.length > 139) {
    throw httpError(400, 'status must be a string of at most 139 characters', 'INVALID_STATUS');
  }
  await sock.updateProfileStatus(status);
  return { status };
}

async function updateProfileName(name) {
  ensureConnected();
  const trimmed = typeof name === 'string' ? name.trim() : '';
  if (!trimmed || trimmed.length > 25) {
    throw httpError(400, 'name must be 1-25 characters', 'INVALID_NAME');
  }
  await sock.updateProfileName(trimmed);
  return { name: trimmed };
}

async function checkOnWhatsApp(phone) {
  ensureConnected();
  const jid = normalizeJid(phone);
  const results = await sock.onWhatsApp(jid);
  const hit = (results || [])[0];
  return {
    input: phone,
    jid: hit?.jid || jid,
    exists: Boolean(hit?.exists),
    lid: hit?.lid || null,
  };
}

async function getBlocklist() {
  ensureConnected();
  const list = await sock.fetchBlocklist();
  return (list || []).filter(Boolean);
}

async function setBlocked(jid, blocked) {
  ensureConnected();
  jid = normalizeJid(jid);
  await sock.updateBlockStatus(jid, blocked ? 'block' : 'unblock');
  return { jid, blocked: Boolean(blocked) };
}

// ─── Groups ──────────────────────────────────────────────────────────────────

async function getGroupMetadata(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!isGroup(jid)) throw httpError(400, 'Not a group JID', 'NOT_A_GROUP');
  return sock.groupMetadata(jid);
}

async function createGroup(subject, participants) {
  ensureConnected();
  const cleanSubject = typeof subject === 'string' ? subject.trim() : '';
  if (!cleanSubject) throw httpError(400, 'subject is required', 'SUBJECT_REQUIRED');
  if (!Array.isArray(participants) || participants.length === 0) {
    throw httpError(400, 'participants[] is required', 'PARTICIPANTS_REQUIRED');
  }
  const jids = participants.map(normalizeJid).filter(Boolean);
  if (jids.length === 0) throw httpError(400, 'No valid participants', 'PARTICIPANTS_REQUIRED');

  const meta = await sock.groupCreate(cleanSubject, jids);
  if (meta?.id) {
    await db.upsertChat({ id: meta.id, name: meta.subject || cleanSubject }, { forceName: true, fromEvent: true });
  }
  return meta;
}

async function updateGroupParticipants(jid, participants, action) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!isGroup(jid)) throw httpError(400, 'Not a group JID', 'NOT_A_GROUP');
  const allowed = ['add', 'remove', 'promote', 'demote'];
  if (!allowed.includes(action)) {
    throw httpError(400, `action must be one of ${allowed.join(', ')}`, 'INVALID_ACTION');
  }
  if (!Array.isArray(participants) || participants.length === 0) {
    throw httpError(400, 'participants[] is required', 'PARTICIPANTS_REQUIRED');
  }
  const results = await sock.groupParticipantsUpdate(jid, participants.map(normalizeJid), action);
  return (results || []).map((r) => ({ jid: r.jid, status: r.status }));
}

async function leaveGroup(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!isGroup(jid)) throw httpError(400, 'Not a group JID', 'NOT_A_GROUP');
  await sock.groupLeave(jid);
  return { left: true };
}

async function getGroupInviteCode(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!isGroup(jid)) throw httpError(400, 'Not a group JID', 'NOT_A_GROUP');
  const code = await sock.groupInviteCode(jid);
  if (!code) throw httpError(502, 'WhatsApp did not return an invite code', 'NO_INVITE_CODE');
  return { code, link: `https://chat.whatsapp.com/${code}` };
}

async function revokeGroupInvite(jid) {
  ensureConnected();
  jid = normalizeJid(jid);
  if (!isGroup(jid)) throw httpError(400, 'Not a group JID', 'NOT_A_GROUP');
  const code = await sock.groupRevokeInvite(jid);
  return code ? { code, link: `https://chat.whatsapp.com/${code}` } : { code: null, link: null };
}

function parseInviteCode(input) {
  let code = String(input || '').trim();
  if (!code) throw httpError(400, 'code is required', 'CODE_REQUIRED');
  const match = /chat\.whatsapp\.com\/([A-Za-z0-9_-]+)/.exec(code);
  if (match) code = match[1];
  code = code.replace(/[?#].*$/, '');
  if (!/^[A-Za-z0-9_-]{8,64}$/.test(code)) {
    throw httpError(400, 'Invalid group invite code', 'INVALID_CODE');
  }
  return code;
}

async function joinGroup(codeOrLink) {
  ensureConnected();
  const code = parseInviteCode(codeOrLink);
  const jid = await sock.groupAcceptInvite(code);
  if (jid) {
    try {
      const meta = await sock.groupMetadata(jid);
      await db.upsertChat({ id: meta.id, name: meta.subject || '' }, { forceName: true, fromEvent: true });
    } catch {}
  }
  return { jid: jid || null, code };
}

async function updateGroupSubject(jid, subject) {
  ensureConnected();
  jid = normalizeJid(jid);
  const cleanSubject = typeof subject === 'string' ? subject.trim() : '';
  if (!cleanSubject) throw httpError(400, 'subject is required', 'SUBJECT_REQUIRED');
  await sock.groupUpdateSubject(jid, cleanSubject);
  await db.upsertChat({ id: jid, name: cleanSubject }, { forceName: true, fromEvent: true });
  return { subject: cleanSubject };
}

async function updateGroupDescription(jid, description) {
  ensureConnected();
  jid = normalizeJid(jid);
  await sock.groupUpdateDescription(jid, description || '');
  return { description: description || '' };
}

async function setGroupSetting(jid, setting) {
  ensureConnected();
  jid = normalizeJid(jid);
  const allowed = ['announcement', 'not_announcement', 'locked', 'unlocked'];
  if (!allowed.includes(setting)) {
    throw httpError(400, `setting must be one of ${allowed.join(', ')}`, 'INVALID_SETTING');
  }
  await sock.groupSettingUpdate(jid, setting);
  return { setting };
}

// ─── Media / profile pictures ────────────────────────────────────────────────

async function getThumbnailOnDemand(chatJid, msgId) {
  const existing = media.getThumbPath(chatJid, msgId);
  if (existing) return existing;
  const raw = await getRawMsg(chatJid, msgId);
  if (!raw) return null;
  return media.saveThumbnailFromRaw(raw);
}

async function downloadMediaOnDemand(chatJid, msgId) {
  const existing = await media.getMediaPath(chatJid, msgId);
  if (existing && !existing.isThumbnail) return existing;

  const rawMsg = await getRawMsg(chatJid, msgId);
  if (!rawMsg) return null;

  const result = await media.downloadAndSave(rawMsg, sock);
  if (result) {
    await db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype, result.fileName);
    return result;
  }
  return null;
}

async function getProfilePicUrl(jid, type = 'image') {
  if (!isConnected()) return null;
  try {
    return await sock.profilePictureUrl(normalizeJid(jid), type === 'preview' ? 'preview' : 'image');
  } catch { return null; }
}

// ─── Status / lifecycle ──────────────────────────────────────────────────────

function getStatus() {
  return {
    status: connectionStatus,
    user: sock?.user ? { id: sock.user.id, name: sock.user.name || '', phoneNumber: phoneFromJid(sock.user.id) } : null,
    historySyncComplete,
  };
}

function getQR() { return latestQR; }

async function stop() {
  manualStop = true;
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  if (cleanupTimer) { clearInterval(cleanupTimer); cleanupTimer = null; }
  if (sock) {
    try { sock.end(undefined); } catch {}
    sock = null;
  }
  latestQR = null;
  historySyncComplete = false;
  connectionStatus = 'stopped';
}

module.exports = {
  start, stop,
  sendText, sendMedia, sendLocation, sendContact,
  deleteForEveryone, deleteForMe, editMessage, sendReaction, forwardMessage,
  markChatRead, markChatUnread,
  setChatArchived, setChatPinned, setChatMuted,
  clearChat, deleteChat,
  sendPresenceUpdate, subscribePresence, setOwnPresence,
  getStatus, getQR, getMe, getAbout,
  updateProfileStatus, updateProfileName,
  checkOnWhatsApp, getBlocklist, setBlocked,
  getGroupMetadata, createGroup, updateGroupParticipants, leaveGroup,
  getGroupInviteCode, revokeGroupInvite, joinGroup, updateGroupSubject, updateGroupDescription,
  setGroupSetting,
  getPresence, getAllPresences,
  downloadMediaOnDemand, getThumbnailOnDemand, getProfilePicUrl,
};
