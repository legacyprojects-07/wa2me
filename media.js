'use strict';

const fs = require('fs');
const path = require('path');
const pino = require('pino');
const { downloadMediaMessage } = require('@whiskeysockets/baileys');
const { unwrapMessage, extensionOf, guessMime } = require('./helpers');
const db = require('./db');

const logger = pino({ level: process.env.LOG_LEVEL || 'silent' });

let MEDIA_DIR = './data/media';

const MEDIA_TYPES = ['image', 'video', 'audio', 'document', 'sticker'];

function init(mediaDir) {
  MEDIA_DIR = path.resolve(mediaDir);
  for (const type of MEDIA_TYPES) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  }
}

function safePart(value) {
  return String(value || '').replace(/[^a-zA-Z0-9._-]/g, '_');
}

function mimeToExt(mimetype, fileName) {
  const map = {
    'image/jpeg': '.jpg', 'image/png': '.png', 'image/webp': '.webp', 'image/gif': '.gif',
    'image/bmp': '.bmp', 'image/heic': '.heic',
    'video/mp4': '.mp4', 'video/3gpp': '.3gp', 'video/webm': '.webm',
    'video/x-matroska': '.mkv',
    'audio/ogg': '.ogg', 'audio/ogg; codecs=opus': '.ogg',
    'audio/mpeg': '.mp3', 'audio/mp4': '.m4a', 'audio/aac': '.aac',
    'audio/amr': '.amr', 'audio/wav': '.wav',
    'application/pdf': '.pdf',
  };
  const normalized = String(mimetype || '').toLowerCase();
  if (map[normalized]) return map[normalized];

  const ext = extensionOf(fileName);
  if (ext && /^[a-z0-9]{1,8}$/.test(ext)) return '.' + ext;

  if (normalized.startsWith('image/')) return '.jpg';
  if (normalized.startsWith('video/')) return '.mp4';
  if (normalized.startsWith('audio/')) return '.ogg';
  return '.bin';
}

function mediaFilePath(mediaType, chatJid, msgId, mimetype, fileName) {
  const safeJid = safePart(chatJid);
  const safeId = safePart(msgId);
  const ext = mimeToExt(mimetype, fileName);
  return path.join(MEDIA_DIR, mediaType || 'document', `${safeJid}_${safeId}${ext}`);
}

function thumbFilePath(chatJid, msgId) {
  const safeJid = safePart(chatJid);
  const safeId = safePart(msgId);
  return path.join(MEDIA_DIR, 'image', `${safeJid}_${safeId}_thumb.jpg`);
}

function detectMediaType(rawMsg) {
  const unwrapped = unwrapMessage(rawMsg && rawMsg.message);
  if (!unwrapped) return null;
  if (unwrapped.imageMessage) return { type: 'image', content: unwrapped.imageMessage };
  if (unwrapped.videoMessage) return { type: 'video', content: unwrapped.videoMessage };
  if (unwrapped.audioMessage) return { type: 'audio', content: unwrapped.audioMessage };
  if (unwrapped.stickerMessage) return { type: 'sticker', content: unwrapped.stickerMessage };
  if (unwrapped.documentMessage) return { type: 'document', content: unwrapped.documentMessage };
  return null;
}

/**
 * Download media from WhatsApp and save to disk.
 * Returns { filePath, mimetype, mediaType, fileName } on success, null on failure.
 */
async function downloadAndSave(rawMsg, sock) {
  if (!rawMsg || !rawMsg.key || !rawMsg.message) return null;

  const chatJid = rawMsg.key.remoteJid;
  const msgId = rawMsg.key.id;
  const detected = detectMediaType(rawMsg);
  if (!detected) return null;

  const mimetype = detected.content.mimetype || guessMime(detected.content.fileName);
  const fileName = detected.content.fileName || null;
  const filePath = mediaFilePath(detected.type, chatJid, msgId, mimetype, fileName);

  if (fs.existsSync(filePath)) {
    const existingThumb = detected.content.jpegThumbnail;
    if (existingThumb) {
      try {
        const tp = thumbFilePath(chatJid, msgId);
        if (!fs.existsSync(tp)) {
          fs.mkdirSync(path.dirname(tp), { recursive: true });
          fs.writeFileSync(tp, Buffer.from(existingThumb));
        }
      } catch {}
    }
    return { filePath, mimetype, mediaType: detected.type, fileName };
  }

  try {
    const downloadOptions = { logger };
    if (sock && typeof sock.updateMediaMessage === 'function') {
      downloadOptions.reuploadRequest = (msg) => sock.updateMediaMessage(msg);
    }
    const buffer = await downloadMediaMessage(rawMsg, 'buffer', {}, downloadOptions);

    if (!buffer || buffer.length === 0) return null;

    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, buffer);

    const thumb = detected.content.jpegThumbnail;
    if (thumb) {
      try { fs.writeFileSync(thumbFilePath(chatJid, msgId), Buffer.from(thumb)); } catch {}
    }

    logger.info(`[Media] Saved ${detected.type} ${chatJid}/${msgId} (${buffer.length} bytes)`);
    return { filePath, mimetype, mediaType: detected.type, fileName };
  } catch (err) {
    logger.warn(`[Media] Download failed ${chatJid}/${msgId}: ${err.message}`);

    const thumb = detected.content.jpegThumbnail;
    if (thumb) {
      try {
        const tp = thumbFilePath(chatJid, msgId);
        if (!fs.existsSync(tp)) fs.writeFileSync(tp, Buffer.from(thumb));
        return { filePath: tp, mimetype: 'image/jpeg', mediaType: 'image', isThumbnail: true };
      } catch {}
    }
    return null;
  }
}

/**
 * Save an uploaded (outgoing) file so the same client can re-open its own
 * sent media, including after a reconnect. Returns { filePath, mimetype }.
 */
async function saveOutgoing(chatJid, msgId, buffer, { mediaType, mimetype, fileName } = {}) {
  if (!buffer || !buffer.length) return null;
  const type = MEDIA_TYPES.includes(mediaType) ? mediaType : 'document';
  const mime = mimetype || guessMime(fileName);
  const filePath = mediaFilePath(type, chatJid, msgId, mime, fileName);
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, buffer);
  return { filePath, mimetype: mime };
}

/**
 * Get the file path for a message's media from DB or disk scan.
 * This is async because it may read the message row and heal the DB.
 */
async function getMediaPath(chatJid, msgId) {
  let msg = null;
  try { msg = await db.getMessage(chatJid, msgId); } catch {}

  if (msg && msg.deleted) return null;

  if (msg && msg.media_path && fs.existsSync(msg.media_path)) {
    return {
      filePath: msg.media_path,
      mimetype: msg.media_mime || guessMime(msg.media_name),
      fileName: msg.media_name || null,
      isThumbnail: false,
    };
  }

  // Disk scan fallback (handles DB inconsistency after crashes).
  const safeJid = safePart(chatJid);
  const safeId = safePart(msgId);
  const prefix = `${safeJid}_${safeId}`;

  for (const type of MEDIA_TYPES) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) continue;
    const match = fs.readdirSync(dir).find((f) =>
      f.startsWith(`${prefix}.`) && !f.includes('_thumb')
    );
    if (match) {
      const fp = path.join(dir, match);
      const mime = msg?.media_mime || guessMime(match);
      if (msg && !msg.media_path) {
        try { await db.updateMessageMedia(chatJid, msgId, fp, mime, msg.media_name); } catch {}
      }
      return { filePath: fp, mimetype: mime, fileName: msg?.media_name || null, isThumbnail: false };
    }
  }

  const tp = thumbFilePath(chatJid, msgId);
  if (fs.existsSync(tp)) {
    return { filePath: tp, mimetype: 'image/jpeg', fileName: null, isThumbnail: true };
  }

  return null;
}

function getThumbPath(chatJid, msgId) {
  const tp = thumbFilePath(chatJid, msgId);
  return fs.existsSync(tp) ? tp : null;
}

/**
 * Extract WhatsApp's inline JPEG thumbnail from a raw protobuf without
 * downloading the full media. Essential for slow J2ME/GPRS connections.
 */
function saveThumbnailFromRaw(rawMsg) {
  if (!rawMsg || !rawMsg.key || !rawMsg.key.remoteJid || !rawMsg.key.id) return null;
  const detected = detectMediaType(rawMsg);
  const thumb = detected?.content?.jpegThumbnail;
  if (!thumb) return null;
  try {
    const tp = thumbFilePath(rawMsg.key.remoteJid, rawMsg.key.id);
    fs.mkdirSync(path.dirname(tp), { recursive: true });
    fs.writeFileSync(tp, Buffer.from(thumb));
    return tp;
  } catch {
    return null;
  }
}

/**
 * Remove all cached files (full media + thumbnail) for a message.
 * Used by delete-for-me and delete-for-everyone so media does not linger.
 */
function deleteMediaFiles(chatJid, msgId) {
  const safeJid = safePart(chatJid);
  const safeId = safePart(msgId);
  const prefix = `${safeJid}_${safeId}`;
  let removed = 0;

  for (const type of MEDIA_TYPES) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) continue;
    for (const file of fs.readdirSync(dir)) {
      if (!file.startsWith(`${prefix}.`) && !file.startsWith(`${prefix}_thumb.`)) continue;
      try { fs.unlinkSync(path.join(dir, file)); removed++; } catch {}
    }
  }
  return removed;
}

/**
 * Remove every cached file belonging to a chat (clear/delete chat).
 */
function deleteChatMediaFiles(chatJid) {
  const prefix = `${safePart(chatJid)}_`;
  let removed = 0;
  for (const type of MEDIA_TYPES) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) continue;
    for (const file of fs.readdirSync(dir)) {
      if (!file.startsWith(prefix)) continue;
      try { fs.unlinkSync(path.join(dir, file)); removed++; } catch {}
    }
  }
  return removed;
}

module.exports = {
  init,
  downloadAndSave,
  saveOutgoing,
  getMediaPath,
  getThumbPath,
  saveThumbnailFromRaw,
  deleteMediaFiles,
  deleteChatMediaFiles,
  mediaFilePath,
  thumbFilePath,
};
