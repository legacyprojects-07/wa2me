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
const uploads = require('./uploads');
const serializers = require('./serializers');
const {
  safeJSON, normalizeJid, httpError, toInt, toBool, firstValue,
  isGroup, parseTimestamp, guessMime, inferMediaType, sanitizeFileName,
} = require('./helpers');

// ─── Config ──────────────────────────────────────────────────────────────────

const PORT = parseInt(process.env.PORT, 10) || 3000;
const AUTH_DIR = path.resolve(process.env.AUTH_DIR || './auth_state');
const MEDIA_DIR = path.resolve(process.env.MEDIA_DIR || './data/media');
const UPLOAD_TMP_DIR = path.resolve(process.env.UPLOAD_TMP_DIR || './data/tmp');
const API_KEY = process.env.API_KEY || '';
const RENDER_URL = process.env.RENDER_EXTERNAL_URL || '';
const JSON_LIMIT = process.env.JSON_BODY_LIMIT || '20mb';
const VERSION = '6.0.0';

// ─── Express ─────────────────────────────────────────────────────────────────

const app = express();
app.disable('x-powered-by');
app.use(express.json({ limit: JSON_LIMIT }));
app.use(express.urlencoded({ extended: true, limit: JSON_LIMIT }));

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, HEAD, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-API-Key, Range, If-Range');
  res.header('Access-Control-Expose-Headers', 'Content-Length, Content-Range, Accept-Ranges, Content-Disposition, X-WA-Status, X-OpenKnot-Thumbnail');
  res.header('X-Content-Type-Options', 'nosniff');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

if (API_KEY) {
  app.use((req, res, next) => {
    if (req.path === '/health' || req.path === '/qr-image' || req.path === '/') return next();
    const key = req.headers['x-api-key'] || req.query.api_key;
    if (key !== API_KEY) return sendJSON(res, { error: 'Unauthorized', code: 'UNAUTHORIZED' }, 401);
    next();
  });
}

function sendJSON(res, data, status = 200) {
  res.status(status).type('application/json').send(safeJSON(data));
}

const ah = (fn) => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

function bodyValue(req, ...names) {
  for (const name of names) {
    const v = firstValue(req.body ? req.body[name] : undefined);
    if (v !== undefined && v !== null && v !== '') return v;
  }
  return undefined;
}

function parseList(value) {
  if (Array.isArray(value)) return value;
  if (value === undefined || value === null || value === '') return [];
  const s = String(value).trim();
  if (s.startsWith('[')) {
    try {
      const parsed = JSON.parse(s);
      if (Array.isArray(parsed)) return parsed;
    } catch {}
  }
  return s.split(/[\s,;]+/).filter(Boolean);
}

function parseLimit(value, fallback, max = 500) {
  const n = toInt(value, fallback);
  return Math.max(1, Math.min(n, max));
}

function parseQueryBool(value, fallback) {
  return toBool(value, fallback);
}

function timestampCursor(value) {
  let n = Number(value);
  if (!Number.isFinite(n) || n < 0) return 0;
  if (n > 0 && n < 100000000000) n *= 1000; // tolerate clients sending seconds
  return Math.floor(n);
}

const DEFAULT_MIME = {
  image: 'image/jpeg',
  video: 'video/mp4',
  audio: 'audio/ogg; codecs=opus',
  document: 'application/octet-stream',
  sticker: 'image/webp',
};

const TYPE_ALIASES = {
  photo: 'image', picture: 'image', img: 'image',
  vid: 'video', movie: 'video',
  voice: 'audio', voicenote: 'audio', 'voice-note': 'audio', ptt: 'audio',
  doc: 'document', file: 'document',
  sticker: 'sticker', stkr: 'sticker',
};

function normalizeMediaType(value) {
  if (value === undefined || value === null || value === '') return null;
  const s = String(value).trim().toLowerCase();
  if (s === 'text' || s === 'none' || s === 'auto') return null;
  const type = TYPE_ALIASES[s] || s;
  if (!Object.prototype.hasOwnProperty.call(DEFAULT_MIME, type)) {
    throw httpError(400, `Invalid mediaType "${value}". Use image, video, audio, document or sticker`, 'INVALID_MEDIA_TYPE');
  }
  return type;
}

function cleanMime(value, type, fileName) {
  let mime = value ? String(value).trim() : '';
  if (!mime || mime === 'application/octet-stream' || mime === 'binary/octet-stream') {
    mime = guessMime(fileName, DEFAULT_MIME[type] || 'application/octet-stream');
  }
  return mime;
}

// ─── Service info / Health / QR ──────────────────────────────────────────────

app.get('/', (req, res) => {
  sendJSON(res, {
    name: 'OpenKnot',
    version: VERSION,
    status: wa.getStatus().status,
    description: 'WhatsApp backend for legacy Qt/Symbian and J2ME clients',
    features: [
      'multipart-upload', 'delete-for-everyone', 'delete-for-me', 'incoming-revoke',
      'reply-quote', 'forward', 'reactions', 'edit', 'voice-notes', 'locations',
      'contact-cards', 'group-management', 'group-settings', 'search', 'delta-sync', 'presence', 'typing',
      'mark-unread', 'archive', 'pin', 'mute', 'clear-chat', 'delete-chat',
      'profile', 'block', 'media-thumbnails', 'range-requests',
    ],
    limits: {
      maxUploadMb: uploads.MAX_UPLOAD_MB,
      maxUploadTotalMb: uploads.MAX_UPLOAD_TOTAL_MB,
      maxFilesPerRequest: uploads.MAX_FILES,
      jsonBodyLimit: JSON_LIMIT,
    },
    time: Math.floor(Date.now() / 1000),
  });
});

app.get('/version', (req, res) => sendJSON(res, { name: 'OpenKnot', version: VERSION }));

app.get('/health', ah(async (req, res) => {
  const s = wa.getStatus();
  let stats = { chats: 0, contacts: 0, messages: 0 };
  try { stats = await db.getStats(); } catch {}
  sendJSON(res, {
    ...s,
    version: VERSION,
    stats,
    limits: {
      maxUploadMb: uploads.MAX_UPLOAD_MB,
      maxUploadTotalMb: uploads.MAX_UPLOAD_TOTAL_MB,
      maxFilesPerRequest: uploads.MAX_FILES,
    },
  });
}));

// Reset auth — clears credentials from PostgreSQL and filesystem, forces fresh QR scan.
app.all('/reset', ah(async (req, res) => {
  console.log('[Reset] Clearing all auth data...');
  await wa.stop();
  console.log('[Reset] WhatsApp connection stopped');

  const { clearAuth } = require('./auth');
  await clearAuth(db.getPool());
  console.log('[Reset] PostgreSQL auth cleared');

  if (fs.existsSync(AUTH_DIR)) {
    fs.rmSync(AUTH_DIR, { recursive: true, force: true });
    console.log('[Reset] Filesystem auth cleared');
  }

  setTimeout(() => {
    wa.start().catch((err) => console.error('[Reset] Restart failed:', err.message));
    console.log('[Reset] WhatsApp restarted — scan QR at /qr-image');
  }, 3000);

  sendJSON(res, {
    ok: true,
    message: 'Auth cleared. Open /qr-image in 5 seconds and scan the new QR code.',
  });
}));

app.get('/qr', ah(async (req, res) => {
  const s = wa.getStatus();
  if (s.status === 'connected') return sendJSON(res, { status: 'connected', qr: null });
  const qr = wa.getQR();
  if (!qr) return sendJSON(res, { status: s.status, error: 'No QR available yet' }, 404);
  sendJSON(res, { status: 'qr', qr: await QRCode.toDataURL(qr, { width: 256, margin: 1 }) });
}));

app.get('/qr-image', ah(async (req, res) => {
  const s = wa.getStatus();
  if (s.status === 'connected') return res.status(404).send('Already connected');
  const qr = wa.getQR();
  if (!qr) return res.status(404).send('No QR yet — refresh in a few seconds');
  let size = parseInt(req.query.size, 10);
  if (!size || size < 80 || size > 500) size = 256;
  res.type('png');
  QRCode.toFileStream(res, qr, { width: size, margin: 1 });
}));

// ─── Chats ───────────────────────────────────────────────────────────────────

app.get('/chats', ah(async (req, res) => {
  const savedOnly = req.query.all !== '1';
  const limit = parseLimit(req.query.limit, 200, 500);
  const offset = Math.max(0, toInt(req.query.offset, 0));
  let archived;
  if (req.query.archived !== undefined && req.query.archived !== '') {
    archived = parseQueryBool(req.query.archived, false);
  }

  res.set('X-WA-Status', wa.getStatus().status);
  const chats = await db.listChats({ savedOnly, limit, offset, archived });
  sendJSON(res, chats.map(serializers.chat));
}));

app.get('/chats/:jid', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const chat = await db.getChat(jid);
  if (!chat) return sendJSON(res, { error: 'Chat not found', code: 'CHAT_NOT_FOUND' }, 404);

  const contact = await db.getContact(jid);
  const presence = wa.getPresence(jid);

  sendJSON(res, {
    ...serializers.chat(chat),
    messageCount: await db.getMessageCount(jid),
    contact: serializers.contact(contact),
    presence,
    connected: wa.getStatus().status === 'connected',
  });
}));

app.post('/chats/:jid/read', ah(async (req, res) => {
  await wa.markChatRead(normalizeJid(decodeURIComponent(req.params.jid)));
  sendJSON(res, { ok: true });
}));

app.post('/chats/:jid/unread', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const chat = await db.getChat(jid);
  if (!chat) return sendJSON(res, { error: 'Chat not found', code: 'CHAT_NOT_FOUND' }, 404);
  const result = await wa.markChatUnread(jid, Math.max(1, toInt(bodyValue(req, 'count'), 1)));
  sendJSON(res, { ok: true, ...result });
}));

app.post('/chats/:jid/typing', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const requested = String(bodyValue(req, 'status') || 'composing').toLowerCase();
  const valid = ['composing', 'paused', 'recording'];
  if (!valid.includes(requested)) {
    return sendJSON(res, { error: `status must be one of ${valid.join(', ')}`, code: 'INVALID_STATUS' }, 400);
  }
  await wa.sendPresenceUpdate(jid, requested);
  sendJSON(res, { ok: true, status: requested });
}));

app.post('/chats/:jid/subscribe', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const result = await wa.subscribePresence(jid);
  sendJSON(res, { ok: true, ...result, presence: wa.getPresence(jid) });
}));

app.post('/chats/:jid/archive', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const archived = parseQueryBool(bodyValue(req, 'archived', 'archive'), true);
  const result = await wa.setChatArchived(jid, archived);
  sendJSON(res, { ok: true, ...result });
}));

app.post('/chats/:jid/pin', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const pinned = parseQueryBool(bodyValue(req, 'pinned', 'pin'), true);
  const result = await wa.setChatPinned(jid, pinned);
  sendJSON(res, { ok: true, ...result });
}));

app.post('/chats/:jid/mute', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const muted = parseQueryBool(bodyValue(req, 'muted', 'mute'), true);
  const duration = toInt(bodyValue(req, 'durationSeconds', 'duration'), 8 * 60 * 60);
  const result = await wa.setChatMuted(jid, muted, duration);
  sendJSON(res, { ok: true, ...result });
}));

app.post('/chats/:jid/clear', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const remote = parseQueryBool(bodyValue(req, 'remote'), req.query.remote === undefined ? true : parseQueryBool(req.query.remote, true));
  const result = await wa.clearChat(jid, { remote });
  sendJSON(res, { ok: true, ...result });
}));

const deleteChatHandler = ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const remote = req.query.remote === undefined ? true : parseQueryBool(req.query.remote, true);
  const result = await wa.deleteChat(jid, { remote });
  sendJSON(res, { ok: true, ...result });
});
app.delete('/chats/:jid', deleteChatHandler);
app.post('/chats/:jid/delete', deleteChatHandler);

// ─── Messages ────────────────────────────────────────────────────────────────

app.get('/messages/:jid', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const limit = parseLimit(req.query.limit, 100, 500);
  const before = req.query.before !== undefined ? toInt(req.query.before, 0) : null;
  const after = req.query.after !== undefined ? toInt(req.query.after, 0) : null;
  const includeDeleted = req.query.includeDeleted === undefined
    ? true
    : parseQueryBool(req.query.includeDeleted, true);
  const markRead = req.query.markRead === undefined ? true : parseQueryBool(req.query.markRead, true);

  if (markRead) await wa.markChatRead(jid);

  let rows;
  if (after !== null) rows = await db.getMessages(jid, { limit, after });
  else if (before !== null) rows = await db.getMessages(jid, { limit, before });
  else rows = await db.getLatestMessages(jid, limit);

  if (!includeDeleted) rows = rows.filter((m) => !m.deleted);
  sendJSON(res, await serializers.decorateMessages(rows));
}));

app.get('/messages/:jid/:msgId', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const row = await db.getMessage(jid, msgId);
  if (!row) return sendJSON(res, { error: 'Message not found', code: 'MESSAGE_NOT_FOUND' }, 404);
  const [out] = await serializers.decorateMessages([row]);
  sendJSON(res, out);
}));

// Express distinguishes this from /messages/:jid/:msgId by the extra path
// segment, so route order is not significant here.
app.get('/messages/:jid/:msgId/media', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);

  const row = await db.getMessage(jid, msgId);
  if (!row) return sendJSON(res, { error: 'Message not found', code: 'MESSAGE_NOT_FOUND' }, 404);
  if (row.deleted) return sendJSON(res, { error: 'Message was deleted', code: 'MESSAGE_DELETED' }, 410);
  if (!row.has_media) return sendJSON(res, { error: 'Message has no media', code: 'NO_MEDIA' }, 404);

  let info = await media.getMediaPath(jid, msgId);
  const wantThumb = req.query.thumb === '1' || req.query.thumb === 'true';

  if (wantThumb) {
    let tp = media.getThumbPath(jid, msgId);
    if (!tp) {
      try { tp = await wa.getThumbnailOnDemand(jid, msgId); } catch {}
    }
    if (tp) {
      res.type('image/jpeg');
      res.set('Cache-Control', 'public, max-age=86400');
      return res.sendFile(path.resolve(tp));
    }
    // Do not pull a full video/image just to answer a thumbnail request.
    if (!info || !info.filePath || !fs.existsSync(info.filePath)) {
      return sendJSON(res, { error: 'No thumbnail available', code: 'NO_THUMBNAIL' }, 404);
    }
    res.set('X-OpenKnot-Thumbnail', 'fallback');
  } else if (!info || info.isThumbnail) {
    try {
      const full = await wa.downloadMediaOnDemand(jid, msgId);
      if (full) info = full;
    } catch {}
  }

  if (!info || !info.filePath || !fs.existsSync(info.filePath)) {
    const tp = media.getThumbPath(jid, msgId);
    if (tp) {
      res.type('image/jpeg');
      res.set('Cache-Control', 'public, max-age=86400');
      return res.sendFile(path.resolve(tp));
    }
    return sendJSON(res, { error: 'Media not available or expired', code: 'MEDIA_UNAVAILABLE' }, 404);
  }

  const mime = info.mimetype || row.media_mime || 'application/octet-stream';
  const fileName = sanitizeFileName(info.fileName || row.media_name || `${msgId}`);
  const download = req.query.download === '1' || req.query.download === 'true';
  const forceAttachment = download || row.media_type === 'document';

  res.type(mime);
  res.set('Cache-Control', 'public, max-age=604800');
  res.set('Accept-Ranges', 'bytes');
  if (info.isThumbnail) res.set('X-OpenKnot-Thumbnail', '1');
  const asciiName = fileName.replace(/[^\x20-\x7e]/g, '_').replace(/[\\"]/g, '_');
  res.set(
    'Content-Disposition',
    `${forceAttachment ? 'attachment' : 'inline'}; filename="${asciiName}"; filename*=UTF-8''${encodeURIComponent(fileName)}`
  );
  res.sendFile(path.resolve(info.filePath));
}));

// Delete for everyone (revoke) or delete for me.
const deleteMessageHandler = ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const requested = String(bodyValue(req, 'scope', 'for') || req.query.scope || 'everyone').toLowerCase();
  const forEveryone = bodyValue(req, 'forEveryone') !== undefined
    ? parseQueryBool(bodyValue(req, 'forEveryone'), true)
    : (requested === 'everyone' || requested === 'all');

  let result;
  if (forEveryone) result = await wa.deleteForEveryone(jid, msgId);
  else result = await wa.deleteForMe(jid, msgId);

  sendJSON(res, {
    ok: true,
    scope: forEveryone ? 'everyone' : 'me',
    ...result,
  });
});
app.post('/messages/:jid/:msgId/delete', deleteMessageHandler);
app.post('/messages/:jid/:msgId/revoke', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  sendJSON(res, { ok: true, scope: 'everyone', ...(await wa.deleteForEveryone(jid, msgId)) });
}));
app.delete('/messages/:jid/:msgId', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const scope = String(req.query.scope || '').toLowerCase();
  const forEveryone = scope === 'everyone' || scope === 'all' || parseQueryBool(req.query.forEveryone, false);
  const result = forEveryone ? await wa.deleteForEveryone(jid, msgId) : await wa.deleteForMe(jid, msgId);
  sendJSON(res, { ok: true, scope: forEveryone ? 'everyone' : 'me', ...result });
}));

app.post('/messages/:jid/:msgId/forward', ah(async (req, res) => {
  const fromJid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const toJid = bodyValue(req, 'to', 'jid', 'target');
  if (!toJid) return sendJSON(res, { error: 'to (target jid) is required', code: 'TO_REQUIRED' }, 400);

  const result = await wa.forwardMessage(fromJid, msgId, normalizeJid(String(toJid)));
  sendJSON(res, {
    ok: true,
    id: result?.key?.id || null,
    timestamp: parseTimestamp(result?.messageTimestamp),
    to: normalizeJid(String(toJid)),
  });
}));

app.post('/messages/:jid/:msgId/react', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const emoji = bodyValue(req, 'emoji', 'reaction', 'text');
  const result = await wa.sendReaction(jid, msgId, emoji === undefined ? '' : String(emoji));
  sendJSON(res, { ok: true, ...result });
}));

app.post('/messages/:jid/:msgId/edit', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const msgId = decodeURIComponent(req.params.msgId);
  const text = bodyValue(req, 'text', 'message', 'caption');
  if (text === undefined) return sendJSON(res, { error: 'text is required', code: 'TEXT_REQUIRED' }, 400);
  const result = await wa.editMessage(jid, msgId, String(text));
  sendJSON(res, { ok: true, ...result });
}));

app.get('/search', ah(async (req, res) => {
  const q = String(req.query.q || '').trim();
  if (!q) return sendJSON(res, { error: 'q is required', code: 'QUERY_REQUIRED' }, 400);
  if (q.length > 200) return sendJSON(res, { error: 'q is too long (max 200)', code: 'QUERY_TOO_LONG' }, 400);

  const chatJid = req.query.jid ? normalizeJid(decodeURIComponent(req.query.jid)) : null;
  const rows = await db.searchMessages(q, {
    chatJid,
    limit: parseLimit(req.query.limit, 100, 300),
    offset: Math.max(0, toInt(req.query.offset, 0)),
  });
  const decorated = await serializers.decorateMessages(rows);
  sendJSON(res, decorated.map((m, i) => ({ ...m, chatName: rows[i]?.chat_name || '' })));
}));

// ─── Delta sync for polling clients ──────────────────────────────────────────

app.get('/sync', ah(async (req, res) => {
  const cursor = timestampCursor(req.query.since || 0);
  const limit = parseLimit(req.query.limit, 200, 500);
  const startedAt = Date.now();

  const [chatRows, messageRows] = await Promise.all([
    db.getChatsUpdatedSince(cursor, limit),
    db.getMessagesUpdatedSince(cursor, limit),
  ]);

  const chats = chatRows.map(serializers.chat);
  const messages = await serializers.decorateMessages(messageRows);
  const hasMore = chatRows.length >= limit || messageRows.length >= limit;

  // If a stream was truncated, rewind to the oldest returned update. The
  // client can call again immediately and will make progress without skipping
  // changes. Boundary rows may repeat; all mutations are idempotent by id.
  let nextSince = startedAt;
  if (hasMore) {
    const stamps = [...chatRows, ...messageRows]
      .map((row) => new Date(row.updated_at).getTime())
      .filter((n) => Number.isFinite(n));
    if (stamps.length) nextSince = Math.max(0, Math.min(...stamps) - 1);
  }

  sendJSON(res, {
    serverTime: startedAt,
    nextSince,
    since: cursor,
    hasMore,
    chats,
    messages,
  });
}));

// ─── Send (JSON base64 or multipart/form-data) ───────────────────────────────

const sendHandler = ah(async (req, res) => {
  try {
    const jidRaw = bodyValue(req, 'jid', 'to', 'chat');
    if (!jidRaw) return sendJSON(res, { error: 'jid is required', code: 'JID_REQUIRED' }, 400);
    const jid = normalizeJid(String(jidRaw));

    const textRaw = bodyValue(req, 'text', 'message');
    const captionRaw = bodyValue(req, 'caption');
    const text = textRaw === undefined ? '' : String(textRaw);
    const caption = captionRaw === undefined ? text : String(captionRaw);
    const quotedId = bodyValue(req, 'quotedId', 'quoted', 'replyTo', 'reply_to');
    const ptt = parseQueryBool(bodyValue(req, 'ptt', 'voice', 'voiceNote'), false);

    let typeField = bodyValue(req, 'mediaType', 'type', 'media_type');
    let overrideMime = bodyValue(req, 'mimetype', 'mime', 'mimeType');
    // Accept the common mistake of putting a MIME type in `type`.
    if (typeField && String(typeField).includes('/')) {
      if (!overrideMime) overrideMime = String(typeField);
      typeField = undefined;
    }
    const requestedType = normalizeMediaType(typeField);
    const fileNameOverride = bodyValue(req, 'fileName', 'filename', 'name');
    const durationSeconds = toInt(bodyValue(req, 'seconds', 'duration', 'durationSeconds'), 0);

    let files = await uploads.readUploadedFiles(req.files);

    if (files.length > 0 && bodyValue(req, 'mediaBase64', 'base64', 'fileBase64', 'data')) {
      throw httpError(400, 'Send either multipart files or mediaBase64, not both', 'AMBIGUOUS_MEDIA');
    }

    if (files.length === 0) {
      const base64 = bodyValue(req, 'mediaBase64', 'base64', 'fileBase64', 'data');
      if (base64) {
        const decoded = uploads.decodeBase64(String(base64));
        const fileName = sanitizeFileName(String(fileNameOverride || 'upload.bin'));
        const declared = overrideMime || decoded.declaredMime || '';
        const type = requestedType || inferMediaType(declared, fileName);
        const mime = cleanMime(declared, type, fileName);
        files = [{ buffer: decoded.buffer, mimetype: mime, fileName, size: decoded.buffer.length }];
      }
    }

    if (files.length > 0) {
      if (wa.getStatus().status !== 'connected') {
        throw httpError(503, `WhatsApp is not connected (${wa.getStatus().status})`, 'NOT_CONNECTED');
      }
      if (files.length > uploads.MAX_FILES) {
        throw httpError(400, `Too many files (max ${uploads.MAX_FILES})`, 'TOO_MANY_FILES');
      }

      const results = [];
      try {
        for (const file of files) {
          const type = requestedType || inferMediaType(file.mimetype, file.fileName);
          if (ptt && type !== 'audio') {
            throw httpError(400, 'ptt=1 is only valid for audio', 'INVALID_PTT');
          }
          const fileName = sanitizeFileName(fileNameOverride ? String(fileNameOverride) : file.fileName);
          const mime = cleanMime(overrideMime || file.mimetype, type, fileName);
          const result = await wa.sendMedia(jid, {
            buffer: file.buffer,
            mediaType: type,
            caption: caption || '',
            mimetype: mime,
            fileName,
            ptt,
            seconds: durationSeconds > 0 ? durationSeconds : undefined,
            quotedId: quotedId ? String(quotedId) : undefined,
          });
          const id = result?.key?.id || null;
          results.push({
            id,
            timestamp: parseTimestamp(result?.messageTimestamp),
            mediaType: type,
            mediaUrl: id ? `/messages/${encodeURIComponent(jid)}/${encodeURIComponent(id)}/media` : null,
            size: file.size,
          });
        }
      } catch (err) {
        // Multi-file sends are not transactional. Return the IDs that were
        // already sent so the client does not blindly resend everything.
        err.sent = results;
        throw err;
      }

      return sendJSON(res, {
        ok: true,
        id: results[0]?.id || null,
        timestamp: results[0]?.timestamp || Math.floor(Date.now() / 1000),
        mediaType: results[0]?.mediaType || null,
        mediaUrl: results[0]?.mediaUrl || null,
        ids: results.map((r) => r.id),
        results,
      });
    }

    if (!text.trim()) {
      return sendJSON(res, {
        error: 'Provide text, a multipart file (field name: file), or mediaBase64',
        code: 'EMPTY_MESSAGE',
      }, 400);
    }

    const result = await wa.sendText(jid, text, { quotedId: quotedId ? String(quotedId) : undefined });
    const id = result?.key?.id || null;
    return sendJSON(res, {
      ok: true,
      id,
      timestamp: parseTimestamp(result?.messageTimestamp),
      mediaType: null,
      mediaUrl: null,
      ids: id ? [id] : [],
      results: [{ id, timestamp: parseTimestamp(result?.messageTimestamp), mediaType: null, mediaUrl: null }],
    });
  } finally {
    // readUploadedFiles already unlinks, but this also covers early errors.
    uploads.cleanupFiles(req.files);
  }
});

app.post('/send', uploads.anyFiles, sendHandler);
app.post('/send-media', uploads.anyFiles, sendHandler);
app.post('/send-file', uploads.anyFiles, sendHandler); // alias for old clients

app.post('/send-contact', uploads.anyFiles, ah(async (req, res) => {
  try {
    const jidRaw = bodyValue(req, 'jid', 'to', 'chat');
    if (!jidRaw) return sendJSON(res, { error: 'jid is required', code: 'JID_REQUIRED' }, 400);
    const name = bodyValue(req, 'name', 'displayName');
    const phone = bodyValue(req, 'phone', 'number', 'phoneNumber');
    if (!name || !phone) {
      return sendJSON(res, { error: 'name and phone are required', code: 'CONTACT_REQUIRED' }, 400);
    }
    const result = await wa.sendContact(normalizeJid(String(jidRaw)), {
      name: String(name),
      phone: String(phone),
      quotedId: bodyValue(req, 'quotedId', 'replyTo'),
    });
    sendJSON(res, {
      ok: true,
      id: result?.key?.id || null,
      timestamp: parseTimestamp(result?.messageTimestamp),
    });
  } finally {
    uploads.cleanupFiles(req.files);
  }
}));

app.post('/send-location', uploads.anyFiles, ah(async (req, res) => {
  try {
    const jidRaw = bodyValue(req, 'jid', 'to', 'chat');
    if (!jidRaw) return sendJSON(res, { error: 'jid is required', code: 'JID_REQUIRED' }, 400);
    const latitude = bodyValue(req, 'latitude', 'lat');
    const longitude = bodyValue(req, 'longitude', 'lng', 'lon');
    if (latitude === undefined || longitude === undefined) {
      return sendJSON(res, { error: 'latitude and longitude are required', code: 'LOCATION_REQUIRED' }, 400);
    }
    const result = await wa.sendLocation(normalizeJid(String(jidRaw)), {
      latitude,
      longitude,
      name: bodyValue(req, 'name', 'label'),
      address: bodyValue(req, 'address'),
      quotedId: bodyValue(req, 'quotedId', 'replyTo'),
    });
    sendJSON(res, {
      ok: true,
      id: result?.key?.id || null,
      timestamp: parseTimestamp(result?.messageTimestamp),
    });
  } finally {
    uploads.cleanupFiles(req.files);
  }
}));

// ─── Profile / users / contacts ──────────────────────────────────────────────

app.get('/me', ah(async (req, res) => {
  const me = wa.getMe();
  if (!me) return sendJSON(res, { error: 'Not connected', code: 'NOT_CONNECTED' }, 503);
  let about = null;
  try { about = await wa.getAbout(me.id); } catch {}
  sendJSON(res, { ...me, about: about?.status || null, profilePic: `/profile-pic/${encodeURIComponent(me.id)}` });
}));

app.post('/me/status', ah(async (req, res) => {
  const status = bodyValue(req, 'status', 'about', 'text');
  if (status === undefined) return sendJSON(res, { error: 'status is required', code: 'STATUS_REQUIRED' }, 400);
  sendJSON(res, { ok: true, ...(await wa.updateProfileStatus(String(status))) });
}));

app.post('/me/name', ah(async (req, res) => {
  const name = bodyValue(req, 'name');
  if (name === undefined) return sendJSON(res, { error: 'name is required', code: 'NAME_REQUIRED' }, 400);
  sendJSON(res, { ok: true, ...(await wa.updateProfileName(String(name))) });
}));

app.post('/me/presence', ah(async (req, res) => {
  const status = String(bodyValue(req, 'status', 'presence') || 'available').toLowerCase();
  sendJSON(res, { ok: true, ...(await wa.setOwnPresence(status)) });
}));

app.get('/user/:jid', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const contact = await db.getContact(jid);
  let about = { status: null, setAt: null };
  try { about = await wa.getAbout(jid); } catch {}
  sendJSON(res, {
    jid,
    contact: serializers.contact(contact),
    about: about.status,
    aboutSetAt: about.setAt,
    presence: wa.getPresence(jid),
    profilePic: `/profile-pic/${encodeURIComponent(jid)}`,
  });
}));

app.get('/check/:phone', ah(async (req, res) => {
  sendJSON(res, await wa.checkOnWhatsApp(decodeURIComponent(req.params.phone)));
}));

app.post('/check', ah(async (req, res) => {
  const phones = parseList(req.body?.phones ?? req.body?.phone);
  if (phones.length === 0) return sendJSON(res, { error: 'phones[] is required', code: 'PHONES_REQUIRED' }, 400);
  if (phones.length > 20) return sendJSON(res, { error: 'Max 20 numbers per request', code: 'TOO_MANY_PHONES' }, 400);
  const results = [];
  for (const phone of phones) results.push(await wa.checkOnWhatsApp(String(phone)));
  sendJSON(res, results);
}));

app.get('/contacts', ah(async (req, res) => {
  const q = String(req.query.q || '').trim().toLowerCase();
  let contacts = db.getAllContacts ? await db.getAllContacts() : [];
  let out = contacts.map(serializers.contact);
  if (q) {
    out = out.filter((c) =>
      String(c.name || '').toLowerCase().includes(q) ||
      String(c.notify || '').toLowerCase().includes(q) ||
      String(c.phoneNumber || '').includes(q) ||
      String(c.jid || '').includes(q)
    );
  }
  sendJSON(res, out);
}));

app.get('/blocked', ah(async (req, res) => {
  const list = await wa.getBlocklist();
  sendJSON(res, list.map((jid) => ({ jid, phoneNumber: String(jid).split('@')[0] })));
}));

app.post('/block/:jid', ah(async (req, res) => {
  sendJSON(res, { ok: true, ...(await wa.setBlocked(normalizeJid(decodeURIComponent(req.params.jid)), true)) });
}));

app.post('/unblock/:jid', ah(async (req, res) => {
  sendJSON(res, { ok: true, ...(await wa.setBlocked(normalizeJid(decodeURIComponent(req.params.jid)), false)) });
}));

// ─── Presence (receive) ──────────────────────────────────────────────────────

app.get('/presence', (req, res) => {
  sendJSON(res, wa.getAllPresences());
});

app.get('/presence/:jid', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  if (req.query.subscribe === '1' && wa.getStatus().status === 'connected') {
    try { await wa.subscribePresence(jid); } catch {}
  }
  sendJSON(res, wa.getPresence(jid));
}));

app.post('/presence', ah(async (req, res) => {
  const status = String(bodyValue(req, 'status', 'presence') || 'available').toLowerCase();
  sendJSON(res, { ok: true, ...(await wa.setOwnPresence(status)) });
}));

// ─── Profile pictures ────────────────────────────────────────────────────────

app.get('/profile-pic/:jid', ah(async (req, res) => {
  if (wa.getStatus().status !== 'connected') {
    return sendJSON(res, { error: 'Not connected', code: 'NOT_CONNECTED' }, 503);
  }
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  try {
    const type = parseQueryBool(req.query.preview, false) ? 'preview' : 'image';
    const url = await wa.getProfilePicUrl(jid, type);
    if (!url) return sendJSON(res, { error: 'No profile picture', code: 'NO_PROFILE_PIC' }, 404);

    const response = await fetch(url, { signal: AbortSignal.timeout(10000) });
    if (!response.ok) return sendJSON(res, { error: 'Failed to fetch profile picture' }, 404);
    const buffer = Buffer.from(await response.arrayBuffer());
    res.type(response.headers.get('content-type') || 'image/jpeg')
      .set('Cache-Control', 'public, max-age=3600')
      .send(buffer);
  } catch {
    sendJSON(res, { error: 'Profile picture unavailable', code: 'PROFILE_PIC_UNAVAILABLE' }, 404);
  }
}));

// ─── Groups ──────────────────────────────────────────────────────────────────

app.get('/groups', ah(async (req, res) => {
  const groups = await db.listGroups(parseLimit(req.query.limit, 200, 500), Math.max(0, toInt(req.query.offset, 0)));
  sendJSON(res, groups.map(serializers.chat));
}));

app.post('/groups', ah(async (req, res) => {
  const subject = bodyValue(req, 'subject', 'name');
  if (!subject) return sendJSON(res, { error: 'subject is required', code: 'SUBJECT_REQUIRED' }, 400);
  const participants = parseList(req.body?.participants ?? req.body?.members);
  const meta = await wa.createGroup(String(subject), participants);
  sendJSON(res, { ok: true, group: serializers.group(meta) });
}));

app.post('/groups/join', ah(async (req, res) => {
  const code = bodyValue(req, 'code', 'link', 'invite');
  if (!code) return sendJSON(res, { error: 'code or link is required', code: 'CODE_REQUIRED' }, 400);
  sendJSON(res, { ok: true, ...(await wa.joinGroup(String(code))) });
}));

app.get('/groups/:jid', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  if (!isGroup(jid)) return sendJSON(res, { error: 'Not a group JID', code: 'NOT_A_GROUP' }, 400);
  const meta = await wa.getGroupMetadata(jid);
  sendJSON(res, serializers.group(meta));
}));

app.post('/groups/:jid/participants', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const action = String(bodyValue(req, 'action') || '').toLowerCase();
  const participants = parseList(req.body?.participants ?? req.body?.members);
  const results = await wa.updateGroupParticipants(jid, participants, action);
  sendJSON(res, { ok: true, action, results });
}));

app.post('/groups/:jid/leave', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  sendJSON(res, { ok: true, ...(await wa.leaveGroup(jid)) });
}));

app.get('/groups/:jid/invite', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  sendJSON(res, { ok: true, ...(await wa.getGroupInviteCode(jid)) });
}));

app.post('/groups/:jid/invite/revoke', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  sendJSON(res, { ok: true, ...(await wa.revokeGroupInvite(jid)) });
}));

app.post('/groups/:jid/subject', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const subject = bodyValue(req, 'subject', 'name');
  if (!subject) return sendJSON(res, { error: 'subject is required', code: 'SUBJECT_REQUIRED' }, 400);
  sendJSON(res, { ok: true, ...(await wa.updateGroupSubject(jid, String(subject))) });
}));

app.post('/groups/:jid/description', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const description = bodyValue(req, 'description', 'desc') || '';
  sendJSON(res, { ok: true, ...(await wa.updateGroupDescription(jid, String(description))) });
}));

app.post('/groups/:jid/settings', ah(async (req, res) => {
  const jid = normalizeJid(decodeURIComponent(req.params.jid));
  const setting = String(bodyValue(req, 'setting', 'action') || '').toLowerCase();
  if (!setting) return sendJSON(res, { error: 'setting is required', code: 'SETTING_REQUIRED' }, 400);
  sendJSON(res, { ok: true, ...(await wa.setGroupSetting(jid, setting)) });
}));

// ─── Maintenance / diagnostics ───────────────────────────────────────────────

app.all('/fix-unread', ah(async (req, res) => {
  await db.getPool().query('UPDATE chats SET unread_count = 0, updated_at = NOW()');
  const { rows } = await db.getPool().query('SELECT COUNT(*)::int AS c FROM chats');
  sendJSON(res, { ok: true, message: `Reset unread count to 0 for ${rows[0].c} chats` });
}));

app.get('/stats', ah(async (req, res) => {
  const stats = await db.getStats();
  sendJSON(res, { ...stats, wa: wa.getStatus() });
}));

// ─── Keep-Alive ──────────────────────────────────────────────────────────────

function startKeepAlive() {
  if (!RENDER_URL) return;
  const url = RENDER_URL + '/health';
  console.log(`[KeepAlive] Pinging ${url} every 10 minutes`);
  setInterval(async () => {
    try { await fetch(url); } catch {}
  }, 10 * 60 * 1000).unref?.();
}

// ─── 404 & Error Handler ─────────────────────────────────────────────────────

app.use((req, res) => {
  sendJSON(res, { error: 'Not found', code: 'NOT_FOUND', path: req.path }, 404);
});

app.use((err, req, res, next) => {
  try { uploads.cleanupFiles(req.files); } catch {}
  let status = err.statusCode || err.status || 500;
  let code = err.code || 'INTERNAL_ERROR';
  let message = err.message || 'Internal server error';

  if (err instanceof multer.MulterError) {
    if (err.code === 'LIMIT_FILE_SIZE') {
      status = 413;
      code = 'FILE_TOO_LARGE';
      message = `File is too large (max ${uploads.MAX_UPLOAD_MB} MB per file)`;
    } else if (err.code === 'LIMIT_FILE_COUNT') {
      status = 400;
      code = 'TOO_MANY_FILES';
      message = `Too many files (max ${uploads.MAX_FILES})`;
    } else {
      status = 400;
      code = err.code || 'UPLOAD_ERROR';
      message = err.message;
    }
  } else if (err.type === 'entity.too.large') {
    status = 413;
    code = 'BODY_TOO_LARGE';
    message = `Request body is too large (JSON limit ${JSON_LIMIT}); use multipart/form-data for media`;
  } else if (err instanceof SyntaxError && err.status === 400 && 'body' in err) {
    status = 400;
    code = 'INVALID_JSON';
    message = 'Invalid JSON body';
  }

  if (status >= 500) {
    console.error('[Express Error]', err);
    // Hide unexpected internal failures, but keep intentional API errors such
    // as 503 NOT_CONNECTED / 502 NO_INVITE_CODE actionable for legacy clients.
    if (process.env.NODE_ENV === 'production' && !err.statusCode) {
      message = 'Internal server error';
      code = 'INTERNAL_ERROR';
    }
  } else {
    console.warn(`[Request Error] ${status} ${code}: ${message}`);
  }

  if (res.headersSent) return next(err);
  const payload = { error: message, code };
  if (Array.isArray(err.sent) && err.sent.length > 0) payload.sent = err.sent;
  sendJSON(res, payload, status);
});

// ─── Boot ────────────────────────────────────────────────────────────────────

async function boot() {
  console.log(`[Boot] OpenKnot Server v${VERSION} (multipart + message actions + sync)`);
  console.log(`[Boot] Auth: ${AUTH_DIR}`);
  console.log(`[Boot] Media: ${MEDIA_DIR}`);
  console.log(`[Boot] Upload temp: ${UPLOAD_TMP_DIR}`);
  if (RENDER_URL) console.log(`[Boot] Render URL: ${RENDER_URL}`);

  await db.init();
  console.log('[Boot] Database connected');

  media.init(MEDIA_DIR);
  uploads.init(UPLOAD_TMP_DIR);
  const stale = uploads.cleanupStaleFiles();
  if (stale) console.log(`[Boot] Removed ${stale} stale upload temp file(s)`);

  app.listen(PORT, '0.0.0.0', () => {
    console.log(`[Boot] Listening on port ${PORT}`);
    startKeepAlive();
  });

  try {
    await wa.start();
  } catch (err) {
    // Keep HTTP alive so /health and /qr-image can still answer. Retry the
    // WhatsApp connection instead of crash-looping the whole web service.
    console.error('[WA] Initial start failed:', err.message);
    const retry = () => {
      wa.start().catch((e) => {
        console.error('[WA] Retry failed:', e.message);
        setTimeout(retry, 15000);
      });
    };
    setTimeout(retry, 15000);
  }
}

let shuttingDown = false;
async function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`[Shutdown] ${signal}`);
  try { await wa.stop(); } catch {}
  try { await db.close(); } catch {}
  process.exit(0);
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));

module.exports = { app, boot };

if (require.main === module) {
  boot().catch((err) => {
    console.error('[Boot Error]', err);
    process.exit(1);
  });
}
