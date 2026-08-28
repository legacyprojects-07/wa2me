'use strict';

require('dotenv').config();

const express = require('express');
const multer = require('multer');
const QRCode = require('qrcode');
const path = require('path');
const fs = require('fs');

const db = require('./db');
const wa = require('./wa');
const media = require('./media');
const { safeJSON, normalizeJid } = require('./helpers');

// ─── Config ──────────────────────────────────────────────────────────────────

const PORT = parseInt(process.env.PORT, 10) || 3000;
const AUTH_DIR = path.resolve(process.env.AUTH_DIR || './auth_state');
const MEDIA_DIR = path.resolve(process.env.MEDIA_DIR || './data/media');
const API_KEY = process.env.API_KEY || '';
const RENDER_URL = process.env.RENDER_EXTERNAL_URL || '';

// ─── Express ─────────────────────────────────────────────────────────────────

const app = express();
app.use(express.json({ limit: '16mb' }));
app.use(express.urlencoded({ extended: true, limit: '16mb' }));

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-API-Key');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

if (API_KEY) {
  app.use((req, res, next) => {
    if (req.path === '/health' || req.path === '/qr-image') return next();
    const key = req.headers['x-api-key'] || req.query.api_key;
    if (key !== API_KEY) return res.status(401).json({ error: 'Unauthorized' });
    next();
  });
}

function sendJSON(res, data, status = 200) {
  res.status(status).type('application/json').send(safeJSON(data));
}

const upload = multer({ limits: { fileSize: 16 * 1024 * 1024 }, storage: multer.memoryStorage() });

// ─── Health & QR ─────────────────────────────────────────────────────────────

app.get('/health', async (req, res) => {
  const s = wa.getStatus();
  let stats = { chats: 0, contacts: 0, messages: 0 };
  try { stats = await db.getStats(); } catch {}
  sendJSON(res, { ...s, stats });
});

app.get('/qr', async (req, res) => {
  const s = wa.getStatus();
  if (s.status === 'connected') return sendJSON(res, { status: 'connected', qr: null });
  const qr = wa.getQR();
  if (!qr) return sendJSON(res, { status: s.status, error: 'No QR available yet' }, 404);
  sendJSON(res, { status: 'qr', qr: await QRCode.toDataURL(qr, { width: 256, margin: 1 }) });
});

app.get('/qr-image', async (req, res) => {
  const s = wa.getStatus();
  if (s.status === 'connected') return res.status(404).send('Already connected');
  const qr = wa.getQR();
  if (!qr) return res.status(404).send('No QR yet — refresh in a few seconds');
  let size = parseInt(req.query.size, 10);
  if (!size || size < 80 || size > 500) size = 256;
  res.type('png');
  QRCode.toFileStream(res, qr, { width: size, margin: 1 });
});

// ─── Chats ───────────────────────────────────────────────────────────────────

app.get('/chats', async (req, res) => {
  const s = wa.getStatus();
  if (s.status !== 'connected') return sendJSON(res, { error: `Not connected (${s.status})` }, 503);

  const savedOnly = req.query.all !== '1';
  const limit = Math.min(parseInt(req.query.limit, 10) || 200, 500);
  const offset = parseInt(req.query.offset, 10) || 0;

  const chats = await db.listChats({ savedOnly, limit, offset });
  sendJSON(res, chats.map(c => ({
    jid: c.jid,
    name: c.name || c.contact_name || c.contact_notify || c.jid.replace(/@.*/, ''),
    isGroup: Boolean(c.is_group),
    unreadCount: c.unread_count,
    lastMessageTs: c.last_message_ts,
    lastMessage: c.last_message,
  })));
});

app.get('/chats/:jid', async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const chat = await db.getChat(jid);
  if (!chat) return sendJSON(res, { error: 'Chat not found' }, 404);

  const contact = await db.getContact(jid);
  const presence = wa.getPresence(jid);

  sendJSON(res, {
    jid: chat.jid,
    name: chat.name || contact?.name || contact?.notify || jid.replace(/@.*/, ''),
    isGroup: Boolean(chat.is_group),
    unreadCount: chat.unread_count,
    lastMessageTs: chat.last_message_ts,
    lastMessage: chat.last_message,
    messageCount: await db.getMessageCount(jid),
    contact: contact ? { name: contact.name, notify: contact.notify, phoneNumber: contact.phone_number, isSaved: Boolean(contact.is_saved) } : null,
    presence: presence,
  });
});

// ─── Messages ────────────────────────────────────────────────────────────────

app.get('/messages/:jid', async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const limit = Math.min(parseInt(req.query.limit, 10) || 100, 500);
  const before = req.query.before ? parseInt(req.query.before, 10) : null;

  await wa.markChatRead(jid);

  const messages = before
    ? await db.getMessages(jid, { limit, before })
    : await db.getLatestMessages(jid, limit);

  sendJSON(res, messages.map(m => ({
    id: m.id,
    fromMe: Boolean(m.from_me),
    senderJid: m.sender_jid,
    senderName: m.sender_name,
    timestamp: m.timestamp,
    text: m.text,
    mediaType: m.media_type,
    hasMedia: Boolean(m.has_media),
    mediaUrl: m.has_media ? `/messages/${encodeURIComponent(jid)}/${encodeURIComponent(m.id)}/media` : null,
    status: m.status,
  })));
});

// ─── Media ───────────────────────────────────────────────────────────────────

app.get('/messages/:jid/:msgId/media', async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);

  let mediaInfo = media.getMediaPath(jid, msgId);

  if (!mediaInfo) {
    try { mediaInfo = await wa.downloadMediaOnDemand(jid, msgId); } catch {}
  }

  if (!mediaInfo || !fs.existsSync(mediaInfo.filePath)) {
    const tp = media.thumbFilePath(jid, msgId);
    if (fs.existsSync(tp)) { res.type('image/jpeg'); res.set('Cache-Control', 'public, max-age=86400'); return res.sendFile(tp); }
    return sendJSON(res, { error: 'Media not available or expired' }, 404);
  }

  if (req.query.thumb === '1') {
    const tp = media.thumbFilePath(jid, msgId);
    if (fs.existsSync(tp)) { res.type('image/jpeg'); res.set('Cache-Control', 'public, max-age=86400'); return res.sendFile(tp); }
  }

  res.type(mediaInfo.mimetype || 'application/octet-stream');
  res.set('Cache-Control', 'public, max-age=604800');
  res.sendFile(path.resolve(mediaInfo.filePath));
});

// ─── Profile Pictures ────────────────────────────────────────────────────────

app.get('/profile-pic/:jid', async (req, res) => {
  const s = wa.getStatus();
  if (s.status !== 'connected') return sendJSON(res, { error: 'Not connected' }, 503);

  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  try {
    const url = await wa.getProfilePicUrl(jid);
    if (!url) return sendJSON(res, { error: 'No profile picture' }, 404);
    const response = await fetch(url);
    if (!response.ok) return sendJSON(res, { error: 'Failed to fetch' }, 404);
    res.type('image/jpeg').set('Cache-Control', 'public, max-age=3600').send(Buffer.from(await response.arrayBuffer()));
  } catch { sendJSON(res, { error: 'Profile picture unavailable' }, 404); }
});

// ─── Presence (online / typing / recording) ──────────────────────────────────

app.get('/presence/:jid', (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const presence = wa.getPresence(jid);
  sendJSON(res, presence);
});

app.get('/presence', (req, res) => {
  const all = wa.getAllPresences();
  sendJSON(res, all);
});

// ─── Send ────────────────────────────────────────────────────────────────────

app.post('/send', async (req, res) => {
  const { jid, text, caption, mediaType, mediaBase64, mimetype, fileName } = req.body || {};
  if (!jid) return sendJSON(res, { error: 'jid is required' }, 400);

  try {
    let result;
    if (mediaBase64 && mediaType) {
      result = await wa.sendMedia(normalizeJid(jid), { buffer: Buffer.from(mediaBase64, 'base64'), mediaType, caption: caption || text || '', mimetype, fileName });
    } else if (text) {
      result = await wa.sendText(normalizeJid(jid), text);
    } else {
      return sendJSON(res, { error: 'Either text or mediaBase64+mediaType required' }, 400);
    }
    sendJSON(res, { ok: true, id: result?.key?.id || null, timestamp: result?.messageTimestamp || Math.floor(Date.now() / 1000) });
  } catch (err) { console.error('[Send Error]', err.message); sendJSON(res, { error: err.message }, 500); }
});

app.post('/send-media', upload.single('file'), async (req, res) => {
  const { jid, caption, mediaType } = req.body || {};
  const file = req.file;
  if (!jid) return sendJSON(res, { error: 'jid is required' }, 400);
  if (!file) return sendJSON(res, { error: 'file is required' }, 400);

  try {
    let type = mediaType;
    if (!type) {
      if (file.mimetype.startsWith('image/')) type = 'image';
      else if (file.mimetype.startsWith('video/')) type = 'video';
      else if (file.mimetype.startsWith('audio/')) type = 'audio';
      else type = 'document';
    }
    const result = await wa.sendMedia(normalizeJid(jid), { buffer: file.buffer, mediaType: type, caption: caption || '', mimetype: file.mimetype, fileName: file.originalname });
    sendJSON(res, { ok: true, id: result?.key?.id || null, timestamp: result?.messageTimestamp || Math.floor(Date.now() / 1000) });
  } catch (err) { console.error('[Send Media Error]', err.message); sendJSON(res, { error: err.message }, 500); }
});

// ─── Mark Read ───────────────────────────────────────────────────────────────

app.post('/chats/:jid/read', async (req, res) => {
  try { await wa.markChatRead(normalizeJid(decodeURIComponent(req.params.jid))); sendJSON(res, { ok: true }); }
  catch (err) { sendJSON(res, { error: err.message }, 500); }
});

// ─── Contacts ────────────────────────────────────────────────────────────────

app.get('/contacts', async (req, res) => {
  const contacts = await db.getAllContacts();
  sendJSON(res, contacts.map(c => ({
    jid: c.jid, name: c.name || c.notify || c.phone_number || c.jid.replace(/@.*/, ''),
    notify: c.notify, phoneNumber: c.phone_number, isSaved: Boolean(c.is_saved),
  })));
});

// ─── Keep-Alive ──────────────────────────────────────────────────────────────

function startKeepAlive() {
  if (!RENDER_URL) return;
  const url = RENDER_URL + '/health';
  console.log(`[KeepAlive] Pinging ${url} every 10 minutes`);
  setInterval(async () => {
    try { await fetch(url); } catch {}
  }, 10 * 60 * 1000);
}

// ─── Error Handler ───────────────────────────────────────────────────────────

app.use((err, req, res, next) => {
  console.error('[Express Error]', err);
  sendJSON(res, { error: 'Internal server error' }, 500);
});

// ─── Boot ────────────────────────────────────────────────────────────────────

async function boot() {
  console.log(`[Boot] WhatsApp Baileys Server v4 (Render PostgreSQL + Presence)`);
  console.log(`[Boot] Auth: ${AUTH_DIR}`);
  console.log(`[Boot] Media: ${MEDIA_DIR}`);
  if (RENDER_URL) console.log(`[Boot] Render URL: ${RENDER_URL}`);

  await db.init();
  console.log('[Boot] Database connected');

  media.init(MEDIA_DIR);

  await wa.start(AUTH_DIR);

  app.listen(PORT, '0.0.0.0', () => {
    console.log(`[Boot] Listening on port ${PORT}`);
    startKeepAlive();
  });
}

process.on('SIGTERM', async () => { console.log('[Shutdown] SIGTERM'); await wa.stop(); await db.close(); process.exit(0); });
process.on('SIGINT', async () => { console.log('[Shutdown] SIGINT'); await wa.stop(); await db.close(); process.exit(0); });

boot().catch(err => { console.error('[Boot Error]', err); process.exit(1); });
