'use strict';

/**
 * Parse Baileys timestamps — handles numbers, strings, and protobuf Long objects.
 */
function parseTimestamp(ts) {
  if (!ts) return Math.floor(Date.now() / 1000);
  if (typeof ts === 'number') return ts;
  if (typeof ts === 'string') return parseInt(ts, 10) || Math.floor(Date.now() / 1000);
  if (typeof ts === 'object' && ts.low !== undefined) return ts.low;
  return Math.floor(Date.now() / 1000);
}

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

/**
 * Unwrap nested WhatsApp message containers:
 * ephemeral, viewOnce, viewOnceV2, documentWithCaption, edited.
 */
function unwrapMessage(message) {
  if (!message) return null;
  if (message.ephemeralMessage) return unwrapMessage(message.ephemeralMessage.message);
  if (message.viewOnceMessage) return unwrapMessage(message.viewOnceMessage.message);
  if (message.viewOnceMessageV2) return unwrapMessage(message.viewOnceMessageV2.message);
  if (message.documentWithCaptionMessage) return unwrapMessage(message.documentWithCaptionMessage.message);
  if (message.editedMessage) return unwrapMessage(message.editedMessage.message);
  if (message.botInvokeMessage) return unwrapMessage(message.botInvokeMessage.message);
  return message;
}

/**
 * Extract text, media type, and media flag from any WhatsApp message payload.
 * Returns null for internal protocol messages that should be ignored.
 */
function parseMessageContent(msg) {
  const content = unwrapMessage(msg.message);
  if (!content) return null;

  if (content.protocolMessage || content.senderKeyDistributionMessage) return null;

  let text = null;
  let mediaType = null;
  let hasMedia = false;
  let mimetype = null;

  if (content.conversation) {
    text = content.conversation;
  } else if (content.extendedTextMessage) {
    text = content.extendedTextMessage.text || '';
  } else if (content.imageMessage) {
    mediaType = 'image';
    hasMedia = true;
    text = content.imageMessage.caption || '';
    mimetype = content.imageMessage.mimetype || 'image/jpeg';
  } else if (content.videoMessage) {
    mediaType = 'video';
    hasMedia = true;
    text = content.videoMessage.caption || '';
    mimetype = content.videoMessage.mimetype || 'video/mp4';
  } else if (content.audioMessage) {
    mediaType = 'audio';
    hasMedia = true;
    text = '';
    mimetype = content.audioMessage.mimetype || 'audio/ogg';
  } else if (content.documentMessage) {
    mediaType = 'document';
    hasMedia = true;
    text = content.documentMessage.caption || '';
    mimetype = content.documentMessage.mimetype || 'application/octet-stream';
  } else if (content.stickerMessage) {
    mediaType = 'sticker';
    hasMedia = true;
    text = '';
    mimetype = content.stickerMessage.mimetype || 'image/webp';
  } else if (content.contactMessage || content.contactsArrayMessage) {
    text = '[Contact]';
  } else if (content.locationMessage || content.liveLocationMessage) {
    text = '[Location]';
  } else if (content.reactionMessage) {
    text = content.reactionMessage.text || '';
  } else {
    text = '';
  }

  return { text, mediaType, hasMedia, mimetype };
}

function isGroup(jid) {
  return jid && jid.endsWith('@g.us');
}

function normalizeJid(jid) {
  if (!jid) return jid;
  jid = jid.trim();
  if (jid.includes('@')) return jid;
  if (/^\d+$/.test(jid)) return jid + '@s.whatsapp.net';
  return jid;
}

module.exports = {
  parseTimestamp,
  safeJSON,
  unwrapMessage,
  parseMessageContent,
  isGroup,
  normalizeJid,
};
