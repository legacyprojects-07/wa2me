'use strict';

const pino = require('pino');
const {
  default: makeWASocket,
  useMultiFileAuthState,
  DisconnectReason,
  fetchLatestBaileysVersion,
} = require('@whiskeysockets/baileys');
const { parseTimestamp, parseMessageContent, isGroup, normalizeJid } = require('./helpers');
const db = require('./db');
const media = require('./media');

let makeCacheableSignalKeyStore, delay;
try { ({ makeCacheableSignalKeyStore } = require('@whiskeysockets/baileys')); } catch {}
try { ({ delay } = require('@whiskeysockets/baileys')); } catch {}
if (!delay) delay = (ms) => new Promise(r => setTimeout(r, ms));

const logger = pino({ level: process.env.LOG_LEVEL || 'silent' });

let sock = null;
let latestQR = null;
let connectionStatus = 'starting';
let historySyncComplete = false;
let reconnectTimer = null;

// Raw message cache for on-demand media downloads
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

function recordMessage(msg, { isHistorySync = false } = {}) {
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
    senderName = msg.pushName || db.resolveName(senderJid, senderJid.replace(/@.*/, ''));
  } else {
    senderJid = chatJid;
    senderName = msg.pushName || db.resolveName(chatJid, '');
  }

  // Record contact from pushName
  if (msg.pushName && !fromMe) {
    const contactJid = isGroup(chatJid) ? (msg.key.participant || '') : chatJid;
    if (contactJid) db.upsertContact({ id: contactJid, notify: msg.pushName, isSaved: false });
  }

  db.insertMessage({
    id: msg.key.id, chatJid, senderJid, senderName, fromMe, timestamp,
    text: parsed.text, mediaType: parsed.mediaType, hasMedia: parsed.hasMedia,
    mediaPath: null, mediaMime: parsed.mimetype, status: fromMe ? 'sent' : 'received',
  });

  const existingChat = db.getChat(chatJid);
  const chatName = existingChat?.name || db.resolveName(chatJid, '');

  db.upsertChat({
    id: chatJid, name: chatName,
    conversationTimestamp: timestamp,
    lastMessage: parsed.text || (parsed.mediaType ? `[${parsed.mediaType}]` : ''),
    unreadCount: 0,
  }, { isInitialSync: isHistorySync });

  if (!fromMe && !isHistorySync) db.incrementUnread(chatJid);
  if (parsed.hasMedia) cacheRawMsg(msg);
}

async function downloadMediaBackground(msg) {
  if (!msg || !msg.key || !sock) return;
  const chatJid = msg.key.remoteJid;
  const msgId = msg.key.id;
  const existing = db.getMessage(chatJid, msgId);
  if (existing && existing.media_path) return;

  try {
    const result = await media.downloadAndSave(msg, sock);
    if (result) db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype);
  } catch (err) {
    logger.warn(`[Media BG] Failed ${chatJid}/${msgId}: ${err.message}`);
  }
}

// ─── WhatsApp Connection ─────────────────────────────────────────────────────

async function start(authDir) {
  const { state, saveCreds } = await useMultiFileAuthState(authDir);
  const { version } = await fetchLatestBaileysVersion();

  const authStore = makeCacheableSignalKeyStore
    ? makeCacheableSignalKeyStore(state, logger) : state;

  sock = makeWASocket({
    auth: authStore, version, logger,
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
        reconnectTimer = setTimeout(() => start(authDir), 5000);
      }
    }
  });

  // ── History Sync ─────────────────────────────────────────────────────────

  sock.ev.on('messaging-history.set', ({ chats: sc, contacts: sco, messages: sm, isLatest }) => {
    console.log(`[WA] History: ${sc?.length || 0} chats, ${sco?.length || 0} contacts, ${sm?.length || 0} msgs`);

    if (sco) for (const c of sco) db.upsertContact({ id: c.id, name: c.name || '', notify: c.notify || '', phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()) });
    if (sc) for (const c of sc) db.upsertChat(c, { isInitialSync: true });
    if (sm && sm.length > 0) {
      const sorted = [...sm].sort((a, b) => parseTimestamp(a.messageTimestamp) - parseTimestamp(b.messageTimestamp));
      for (const msg of sorted) recordMessage(msg, { isHistorySync: true });
    }

    db.reResolveAllChatNames();
    if (isLatest) { historySyncComplete = true; console.log('[WA] History sync complete'); }
  });

  // ── Contact Events ───────────────────────────────────────────────────────

  sock.ev.on('contacts.upsert', (newContacts) => {
    for (const c of (newContacts || [])) db.upsertContact({ id: c.id, name: c.name || '', notify: c.notify || '', phoneNumber: c.phoneNumber || '', isSaved: Boolean(c.name?.trim()) });
    db.reResolveAllChatNames();
  });

  sock.ev.on('contacts.update', (updates) => {
    for (const u of (updates || [])) { if (!u.id) continue; db.upsertContact({ id: u.id, name: u.name || '', notify: u.notify || '', phoneNumber: u.phoneNumber || '', isSaved: Boolean(u.name?.trim()) }); }
    db.reResolveAllChatNames();
  });

  // ── Chat Events ──────────────────────────────────────────────────────────

  sock.ev.on('chats.upsert', (newChats) => { for (const c of (newChats || [])) db.upsertChat(c); });
  sock.ev.on('chats.update', (updates) => { for (const u of (updates || [])) { if (u.id) db.upsertChat({ id: u.id, ...u }); } });
  sock.ev.on('chats.delete', (deleted) => {
    const d = db.getDb();
    for (const jid of (deleted || [])) { d.prepare('DELETE FROM chats WHERE jid = ?').run(jid); d.prepare('DELETE FROM messages WHERE chat_jid = ?').run(jid); }
  });

  // ── Message Events ───────────────────────────────────────────────────────

  sock.ev.on('messages.upsert', ({ messages: msgs }) => {
    for (const m of (msgs || [])) {
      recordMessage(m);
      const parsed = parseMessageContent(m);
      if (parsed && parsed.hasMedia && !m.key.fromMe) downloadMediaBackground(m);
    }
  });

  sock.ev.on('messages.update', (updates) => {
    for (const item of (updates || [])) {
      if (!item.key) continue;
      const { remoteJid, id } = item.key;
      const upd = item.update || {};
      if (upd.status !== undefined) {
        const statusMap = { 0: 'error', 1: 'pending', 2: 'sent', 3: 'delivered', 4: 'read' };
        db.getDb().prepare('UPDATE messages SET status = ? WHERE chat_jid = ? AND id = ?').run(statusMap[upd.status] || 'received', remoteJid, id);
      }
      if (upd.message === null) db.deleteMessage(remoteJid, id);
    }
  });

  // ── Cache Cleanup ────────────────────────────────────────────────────────

  setInterval(() => {
    const cutoff = Date.now() - RAW_CACHE_TTL;
    let n = 0;
    for (const [k, v] of rawMsgCache) { if (v.ts < cutoff) { rawMsgCache.delete(k); n++; } }
    if (n) logger.info(`[Cache] Evicted ${n} entries`);
  }, 3600000);
}

// ─── API Functions ───────────────────────────────────────────────────────────

async function sendText(jid, text) {
  if (!sock || connectionStatus !== 'connected') throw new Error(`Not connected (${connectionStatus})`);
  jid = normalizeJid(jid);
  await sock.presenceUpdate('composing', jid);
  const result = await sock.sendMessage(jid, { text });
  await sock.presenceUpdate('paused', jid);
  if (result?.key) recordMessage(result);
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
  if (result?.key) recordMessage(result);
  return result;
}

async function markChatRead(jid) {
  jid = normalizeJid(jid);
  db.setUnreadCount(jid, 0);
  if (sock && connectionStatus === 'connected') {
    try {
      const msgs = db.getLatestMessages(jid, 5).filter(m => !m.from_me).map(m => ({ key: { remoteJid: jid, id: m.id, fromMe: false }, messageTimestamp: m.timestamp }));
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
  if (result) { db.updateMessageMedia(chatJid, msgId, result.filePath, result.mimetype); return { filePath: result.filePath, mimetype: result.mimetype }; }
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
};
