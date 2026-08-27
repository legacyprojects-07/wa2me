'use strict';

const fs = require('fs');
const path = require('path');
const pino = require('pino');
const { downloadMediaMessage } = require('@whiskeysockets/baileys');
const { unwrapMessage } = require('./helpers');
const db = require('./db');

const logger = pino({ level: process.env.LOG_LEVEL || 'silent' });

let MEDIA_DIR = './data/media';

function init(mediaDir) {
  MEDIA_DIR = mediaDir;
  for (const type of ['image', 'video', 'audio', 'document', 'sticker']) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  }
}

function mimeToExt(mimetype) {
  const map = {
    'image/jpeg': '.jpg', 'image/png': '.png', 'image/webp': '.webp', 'image/gif': '.gif',
    'video/mp4': '.mp4', 'video/3gpp': '.3gp',
    'audio/ogg': '.ogg', 'audio/mpeg': '.mp3', 'audio/mp4': '.m4a', 'audio/aac': '.aac',
    'application/pdf': '.pdf',
  };
  return map[mimetype] || '.bin';
}

function mediaFilePath(mediaType, chatJid, msgId, mimetype) {
  const safeJid = chatJid.replace(/[^a-zA-Z0-9._-]/g, '_');
  const ext = mimeToExt(mimetype);
  return path.join(MEDIA_DIR, mediaType || 'document', `${safeJid}_${msgId}${ext}`);
}

function thumbFilePath(chatJid, msgId) {
  const safeJid = chatJid.replace(/[^a-zA-Z0-9._-]/g, '_');
  return path.join(MEDIA_DIR, 'image', `${safeJid}_${msgId}_thumb.jpg`);
}

function detectMediaType(rawMsg) {
  const unwrapped = unwrapMessage(rawMsg.message);
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
 * Returns { filePath, mimetype, mediaType } on success, null on failure.
 */
async function downloadAndSave(rawMsg, sock) {
  if (!rawMsg || !rawMsg.key || !rawMsg.message) return null;

  const chatJid = rawMsg.key.remoteJid;
  const msgId = rawMsg.key.id;
  const detected = detectMediaType(rawMsg);
  if (!detected) return null;

  const mimetype = detected.content.mimetype || 'application/octet-stream';
  const filePath = mediaFilePath(detected.type, chatJid, msgId, mimetype);

  // Already on disk?
  if (fs.existsSync(filePath)) {
    return { filePath, mimetype, mediaType: detected.type };
  }

  try {
    const buffer = await downloadMediaMessage(
      rawMsg, 'buffer', {},
      { logger, reuploadRequest: sock.updateMediaMessage }
    );

    if (!buffer || buffer.length === 0) return null;

    const dir = path.dirname(filePath);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(filePath, buffer);

    // Save thumbnail if available
    const thumb = detected.content.jpegThumbnail;
    if (thumb) {
      fs.writeFileSync(thumbFilePath(chatJid, msgId), Buffer.from(thumb));
    }

    logger.info(`[Media] Saved ${detected.type} ${chatJid}/${msgId} (${buffer.length} bytes)`);
    return { filePath, mimetype, mediaType: detected.type };
  } catch (err) {
    logger.warn(`[Media] Download failed ${chatJid}/${msgId}: ${err.message}`);

    // Fallback to thumbnail
    const thumb = detected.content.jpegThumbnail;
    if (thumb) {
      const tp = thumbFilePath(chatJid, msgId);
      if (!fs.existsSync(tp)) fs.writeFileSync(tp, Buffer.from(thumb));
      return { filePath: tp, mimetype: 'image/jpeg', mediaType: 'image', isThumbnail: true };
    }
    return null;
  }
}

/**
 * Get the file path for a message's media from DB or disk scan.
 */
function getMediaPath(chatJid, msgId) {
  const msg = db.getMessage(chatJid, msgId);
  if (msg && msg.media_path && fs.existsSync(msg.media_path)) {
    return { filePath: msg.media_path, mimetype: msg.media_mime };
  }

  // Disk scan fallback (handles DB inconsistency after crashes)
  const safeJid = chatJid.replace(/[^a-zA-Z0-9._-]/g, '_');
  for (const type of ['image', 'video', 'audio', 'document', 'sticker']) {
    const dir = path.join(MEDIA_DIR, type);
    if (!fs.existsSync(dir)) continue;
    const match = fs.readdirSync(dir).find(f => f.startsWith(`${safeJid}_${msgId}`) && !f.includes('_thumb'));
    if (match) {
      const fp = path.join(dir, match);
      if (msg && !msg.media_path) {
        const ext = path.extname(match);
        const mimeMap = { '.jpg': 'image/jpeg', '.png': 'image/png', '.mp4': 'video/mp4', '.ogg': 'audio/ogg' };
        db.updateMessageMedia(chatJid, msgId, fp, mimeMap[ext] || 'application/octet-stream');
      }
      return { filePath: fp, mimetype: msg?.media_mime || 'application/octet-stream' };
    }
  }

  // Thumbnail fallback
  const tp = thumbFilePath(chatJid, msgId);
  if (fs.existsSync(tp)) return { filePath: tp, mimetype: 'image/jpeg' };

  return null;
}

module.exports = { init, downloadAndSave, getMediaPath, mediaFilePath, thumbFilePath };
