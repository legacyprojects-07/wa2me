'use strict';

/**
 * Shared helpers for OpenKnot.
 *
 * These functions are intentionally dependency-free so they can be unit tested
 * without a database or a live WhatsApp connection.
 */

// ─── Errors ──────────────────────────────────────────────────────────────────

/**
 * Create an Error that Express will turn into a JSON response with a status.
 * @param {number} statusCode
 * @param {string} message
 * @param {string} [code]
 */
function httpError(statusCode, message, code) {
  const err = new Error(message);
  err.statusCode = statusCode;
  err.code = code || 'OPENKNOT_ERROR';
  return err;
}

// ─── Time / numbers ──────────────────────────────────────────────────────────

/**
 * Parse Baileys timestamps — handles seconds, milliseconds, strings and
 * protobuf Long objects. Always returns Unix seconds.
 */
function parseTimestamp(ts) {
  if (ts === null || ts === undefined || ts === '') {
    return Math.floor(Date.now() / 1000);
  }

  let n;
  if (typeof ts === 'number') n = ts;
  else if (typeof ts === 'string') n = parseInt(ts, 10);
  else if (typeof ts === 'object' && ts.low !== undefined) n = Number(ts.low);
  else if (typeof ts === 'object' && typeof ts.toNumber === 'function') n = ts.toNumber();

  if (!Number.isFinite(n) || n < 0) return Math.floor(Date.now() / 1000);
  if (n === 0) return 0;
  // Some servers/clients send milliseconds. Seconds stay far below this cutoff.
  if (n > 100000000000) n = Math.floor(n / 1000);
  return Math.floor(n);
}

function toInt(value, fallback = 0) {
  const n = parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

function toBool(value, fallback = false) {
  if (value === undefined || value === null || value === '') return fallback;
  if (typeof value === 'boolean') return value;
  const s = String(value).trim().toLowerCase();
  if (['1', 'true', 'yes', 'on', 'y'].includes(s)) return true;
  if (['0', 'false', 'no', 'off', 'n'].includes(s)) return false;
  return fallback;
}

/** Multer may put repeated form fields in an array. Return the first value. */
function firstValue(value) {
  return Array.isArray(value) ? value[0] : value;
}

// ─── JIDs ────────────────────────────────────────────────────────────────────

function isGroup(jid) {
  return Boolean(jid && jid.endsWith('@g.us'));
}

function isStatusBroadcast(jid) {
  return !jid || jid === 'status@broadcast' || jid.endsWith('@newsletter');
}

/**
 * Normalize phone JIDs. Accepts "+91 98123 45678", "919812345678",
 * "919812345678@s.whatsapp.net" and group JIDs unchanged.
 */
function normalizeJid(jid) {
  if (!jid) return jid;
  jid = String(jid).trim();
  if (jid.includes('@')) return jid;

  const cleaned = jid.replace(/[\s()+.-]/g, '');
  if (/^\d{5,}$/.test(cleaned)) return `${cleaned}@s.whatsapp.net`;
  return jid;
}

function phoneFromJid(jid) {
  if (!jid) return '';
  return String(jid).split('@')[0].split(':')[0];
}

// ─── MIME / filenames ────────────────────────────────────────────────────────

const MIME_BY_EXT = {
  jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif',
  webp: 'image/webp', bmp: 'image/bmp', heic: 'image/heic',
  mp4: 'video/mp4', m4v: 'video/mp4', '3gp': 'video/3gpp', '3gpp': 'video/3gpp',
  mkv: 'video/x-matroska', webm: 'video/webm',
  ogg: 'audio/ogg', oga: 'audio/ogg', opus: 'audio/ogg', mp3: 'audio/mpeg',
  m4a: 'audio/mp4', aac: 'audio/aac', amr: 'audio/amr', wav: 'audio/wav',
  pdf: 'application/pdf', zip: 'application/zip',
  doc: 'application/msword',
  docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
  xls: 'application/vnd.ms-excel',
  xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  ppt: 'application/vnd.ms-powerpoint',
  pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  txt: 'text/plain', csv: 'text/csv', vcf: 'text/vcard', apk: 'application/vnd.android.package-archive',
};

function extensionOf(name) {
  const base = String(name || '').split(/[?#]/)[0];
  const idx = base.lastIndexOf('.');
  return idx >= 0 ? base.slice(idx + 1).toLowerCase() : '';
}

function guessMime(name, fallback) {
  return MIME_BY_EXT[extensionOf(name)] || fallback || 'application/octet-stream';
}

/**
 * Infer the WhatsApp media kind from a MIME type / file name.
 * Returns one of image|video|audio|sticker|document.
 */
function inferMediaType(mimetype, fileName) {
  const mime = String(mimetype || '').toLowerCase();
  // WebP defaults to image. A sticker must be requested explicitly with
  // mediaType=sticker, because arbitrary WebP photos are not valid stickers.
  if (mime.startsWith('image/')) return 'image';
  if (mime.startsWith('video/')) return 'video';
  if (mime.startsWith('audio/')) return 'audio';

  const ext = extensionOf(fileName);
  if (['jpg', 'jpeg', 'png', 'gif', 'bmp', 'heic'].includes(ext)) return 'image';
  if (['mp4', '3gp', '3gpp', 'mkv', 'webm', 'm4v'].includes(ext)) return 'video';
  if (['ogg', 'oga', 'opus', 'mp3', 'm4a', 'aac', 'amr', 'wav'].includes(ext)) return 'audio';
  if (ext === 'webp') return 'image';
  return 'document';
}

/**
 * Make a filename safe to store in Content-Disposition and PostgreSQL.
 * Keeps Unicode letters (J2ME file names are often transliterated, but Qt
 * clients may send Devanagari), strips slashes/control characters.
 */
function sanitizeFileName(name, fallback = 'file') {
  let out = String(name || '').replace(/\\/g, '/').split('/').pop();
  out = out.replace(/[\u0000-\u001f\u007f]/g, '').trim();
  if (!out || out === '.' || out === '..') out = fallback;
  if (out.length > 180) {
    const dot = out.lastIndexOf('.');
    const ext = dot > 0 ? out.slice(dot) : '';
    out = out.slice(0, Math.max(1, 180 - ext.length)) + ext;
  }
  return out;
}

// ─── Message content ─────────────────────────────────────────────────────────

/**
 * Unwrap nested WhatsApp message containers:
 * ephemeral, viewOnce, viewOnceV2, documentWithCaption, edited.
 */
function unwrapMessage(message) {
  if (!message) return null;
  if (message.ephemeralMessage) return unwrapMessage(message.ephemeralMessage.message);
  if (message.viewOnceMessage) return unwrapMessage(message.viewOnceMessage.message);
  if (message.viewOnceMessageV2) return unwrapMessage(message.viewOnceMessageV2.message);
  if (message.viewOnceMessageV2Extension) return unwrapMessage(message.viewOnceMessageV2Extension.message);
  if (message.documentWithCaptionMessage) return unwrapMessage(message.documentWithCaptionMessage.message);
  if (message.editedMessage) return unwrapMessage(message.editedMessage.message);
  if (message.botInvokeMessage) return unwrapMessage(message.botInvokeMessage.message);
  return message;
}

function getContextInfo(content) {
  if (!content) return null;
  return content.extendedTextMessage?.contextInfo
    || content.imageMessage?.contextInfo
    || content.videoMessage?.contextInfo
    || content.audioMessage?.contextInfo
    || content.documentMessage?.contextInfo
    || content.stickerMessage?.contextInfo
    || content.locationMessage?.contextInfo
    || content.contactMessage?.contextInfo
    || content.contactsArrayMessage?.contextInfo
    || null;
}

/**
 * Extract text, media metadata and quote info from any WhatsApp payload.
 * Returns null for protocol/reaction messages (those have their own events).
 */
function parseMessageContent(msg) {
  const content = unwrapMessage(msg && msg.message);
  if (!content) return null;
  if (content.protocolMessage || content.senderKeyDistributionMessage) return null;
  if (content.reactionMessage) return null;

  let text = '';
  let mediaType = null;
  let hasMedia = false;
  let mimetype = null;
  let fileName = null;
  let seconds = null;
  let ptt = false;
  let kind = 'text';
  let storeRaw = false;

  if (content.conversation) {
    text = content.conversation;
  } else if (content.extendedTextMessage) {
    text = content.extendedTextMessage.text || '';
  } else if (content.imageMessage) {
    kind = mediaType = 'image';
    hasMedia = storeRaw = true;
    text = content.imageMessage.caption || '';
    mimetype = content.imageMessage.mimetype || 'image/jpeg';
  } else if (content.videoMessage) {
    kind = mediaType = 'video';
    hasMedia = storeRaw = true;
    text = content.videoMessage.caption || '';
    mimetype = content.videoMessage.mimetype || 'video/mp4';
    seconds = content.videoMessage.seconds || null;
  } else if (content.audioMessage) {
    kind = mediaType = 'audio';
    hasMedia = storeRaw = true;
    text = '';
    mimetype = content.audioMessage.mimetype || 'audio/ogg';
    seconds = content.audioMessage.seconds || null;
    ptt = Boolean(content.audioMessage.ptt);
  } else if (content.documentMessage) {
    kind = mediaType = 'document';
    hasMedia = storeRaw = true;
    text = content.documentMessage.caption || '';
    mimetype = content.documentMessage.mimetype || 'application/octet-stream';
    fileName = content.documentMessage.fileName || null;
  } else if (content.stickerMessage) {
    kind = mediaType = 'sticker';
    hasMedia = storeRaw = true;
    text = '';
    mimetype = content.stickerMessage.mimetype || 'image/webp';
    ptt = false;
  } else if (content.contactMessage) {
    kind = 'contact';
    storeRaw = true;
    text = '[Contact]' + (content.contactMessage.displayName ? `: ${content.contactMessage.displayName}` : '');
  } else if (content.contactsArrayMessage) {
    kind = 'contact';
    storeRaw = true;
    const n = content.contactsArrayMessage.contacts?.length || 0;
    text = n > 1 ? `[${n} Contacts]` : '[Contact]';
  } else if (content.locationMessage || content.liveLocationMessage) {
    kind = 'location';
    storeRaw = true;
    const loc = content.locationMessage || content.liveLocationMessage;
    text = loc.name || loc.address || '[Location]';
  } else if (content.pollCreationMessage || content.pollCreationMessageV2 || content.pollCreationMessageV3) {
    kind = 'poll';
    const poll = content.pollCreationMessage || content.pollCreationMessageV2 || content.pollCreationMessageV3;
    text = `[Poll] ${poll.name || ''}`.trim();
  } else if (content.eventMessage) {
    kind = 'event';
    text = `[Event] ${content.eventMessage.name || ''}`.trim();
  } else if (content.albumMessage) {
    kind = 'album';
    text = '[Album]';
  } else {
    // Stubs, buttons, lists, protocol-ish payloads. Keep a readable fallback
    // instead of inserting an empty invisible row.
    // Unknown/unsupported payload: return null rather than storing an empty
    // invisible row. Media/contact/location payloads are handled above.
    return null;
  }

  const ctx = getContextInfo(content);
  const quotedId = ctx?.stanzaId || null;

  return { text, mediaType, hasMedia, mimetype, fileName, seconds, ptt, kind, quotedId, storeRaw };
}

// ─── Raw protobuf persistence ────────────────────────────────────────────────

const B64_MARKER = '__openknot_b64__';
const BIGINT_MARKER = '__openknot_bigint__';

/**
 * Convert a Baileys WAMessage (which contains Buffer/Uint8Array fields) into a
 * JSONB-safe object so media can be re-downloaded after a cold start.
 *
 * NOTE: do not use JSON.stringify's replacer for Buffer detection. Buffer has
 * a toJSON() method, so JSON.stringify turns it into {type:'Buffer',data:[]}
 * before the replacer runs. We walk the object ourselves instead.
 */
function encodeRawMessage(obj, seen = new WeakSet(), depth = 0) {
  if (obj === null || obj === undefined) return null;
  if (depth > 80) return null;

  if (typeof obj === 'bigint') return { [BIGINT_MARKER]: obj.toString() };
  if (typeof obj === 'string' || typeof obj === 'number' || typeof obj === 'boolean') return obj;
  if (typeof obj === 'function' || typeof obj === 'symbol') return undefined;

  if (Buffer.isBuffer(obj) || obj instanceof Uint8Array) {
    return { [B64_MARKER]: Buffer.from(obj).toString('base64') };
  }
  if (obj instanceof Date) return Math.floor(obj.getTime() / 1000);
  if (Array.isArray(obj)) return obj.map((v) => encodeRawMessage(v, seen, depth + 1));
  if (typeof obj !== 'object') return null;

  try {
    if (seen.has(obj)) return null;
    seen.add(obj);
    const out = {};
    for (const [key, value] of Object.entries(obj)) {
      const encoded = encodeRawMessage(value, seen, depth + 1);
      if (encoded !== undefined) out[key] = encoded;
    }
    return out;
  } catch {
    return null;
  }
}

/** Restore Buffers encoded by encodeRawMessage. */
function decodeRawMessage(obj) {
  if (!obj || typeof obj !== 'object') return null;
  if (typeof obj[B64_MARKER] === 'string') {
    return Buffer.from(obj[B64_MARKER], 'base64');
  }
  if (typeof obj[BIGINT_MARKER] === 'string') {
    try { return BigInt(obj[BIGINT_MARKER]); } catch { return null; }
  }
  if (Array.isArray(obj)) return obj.map(decodeRawMessage);
  const out = {};
  for (const [key, value] of Object.entries(obj)) out[key] = decodeRawMessage(value);
  return out;
}

// ─── HTTP / JSON ─────────────────────────────────────────────────────────────

/**
 * ASCII-safe JSON serializer for Symbian / J2ME clients.
 * Escapes all non-ASCII to \uXXXX to prevent UTF-8 corruption.
 */
function safeJSON(obj) {
  const raw = JSON.stringify(obj);
  return raw.replace(/[\u0080-\uFFFF]/g, (ch) => {
    const hex = ch.charCodeAt(0).toString(16).toUpperCase();
    return '\\u' + '0000'.substring(0, 4 - hex.length) + hex;
  });
}

module.exports = {
  httpError,
  parseTimestamp,
  toInt,
  toBool,
  firstValue,
  isGroup,
  isStatusBroadcast,
  normalizeJid,
  phoneFromJid,
  extensionOf,
  guessMime,
  inferMediaType,
  sanitizeFileName,
  unwrapMessage,
  getContextInfo,
  parseMessageContent,
  encodeRawMessage,
  decodeRawMessage,
  safeJSON,
};
