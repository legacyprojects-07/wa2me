require('dotenv').config();
const express = require('express');
const pino = require('pino');
const QRCode = require('qrcode');
const multer = require('multer');
const {
  default: makeWASocket,
  useMultiFileAuthState,
  DisconnectReason,
  fetchLatestBaileysVersion,
  downloadMediaMessage,
} = require('@whiskeysockets/baileys');

const app = express();
const PORT = process.env.PORT || 3000;
const AUTH_DIR = process.env.AUTH_DIR || './auth_state';

let sock = null;
let latestQR = null;
let connectionStatus = 'starting'; // starting | qr | connected | disconnected | loggedOut

// In-memory store rebuilt from Baileys events
const chats = new Map(); // jid -> { id, name, unreadCount, lastMessageTs }
const contacts = new Map(); // jid -> { name, notify, phoneNumber, lid }
const messages = new Map(); // jid -> array of recent messages (capped)
const MAX_MESSAGES_PER_CHAT = 500;
const pendingHistoryRequests = new Set();

// Helper to reliably parse Baileys timestamps (handles numbers, strings, and protobuf Long objects)
function parseTimestamp(ts) {
  if (!ts) return Math.floor(Date.now() / 1000);
  if (typeof ts === 'number') return ts;
  if (typeof ts === 'string') return parseInt(ts, 10) || Math.floor(Date.now() / 1000);
  if (typeof ts === 'object' && ts.low !== undefined) {
    return ts.low;
  }
  return Math.floor(Date.now() / 1000);
}

// ASCII-safe JSON serializer for Symbian Belle / S40 J2ME clients
// Prevents Symbian UTF-8 multibyte character corruption on emoji and non-ASCII strings
function safeJSONStringify(obj) {
  const raw = JSON.stringify(obj);
  return raw.replace(/[\u0080-\uFFFF]/g, (ch) => {
    const hex = ch.charCodeAt(0).toString(16).toUpperCase();
    return '\\u' + '0000'.substring(0, 4 - hex.length) + hex;
  });
}

// Helper to unwrap nested WhatsApp message containers (ephemeral, viewOnce, edited, documentWithCaption)
function unwrapMessage(message) {
  if (!message) return null;
  if (message.ephemeralMessage) return unwrapMessage(message.ephemeralMessage.message);
  if (message.viewOnceMessage) return unwrapMessage(message.viewOnceMessage.message);
  if (message.viewOnceMessageV2) return unwrapMessage(message.viewOnceMessageV2.message);
  if (message.documentWithCaptionMessage) return unwrapMessage(message.documentWithCaptionMessage.message);
  if (message.editedMessage) return unwrapMessage(message.editedMessage.message);
  return message;
}

// Helper to extract text and media type from any WhatsApp message payload
function parseMessageContent(msg) {
  const content = unwrapMessage(msg.message);
  if (!content) return { text: '[Message]', mediaType: null, hasMedia: false };

  let text = null;
  let mediaType = null;
  let hasMedia = false;

  if (content.conversation) {
    text = content.conversation;
  } else if (content.extendedTextMessage?.text) {
    text = content.extendedTextMessage.text;
  } else if (content.imageMessage) {
    mediaType = 'image';
    hasMedia = true;
    text = content.imageMessage.caption || '[Photo]';
  } else if (content.videoMessage) {
    mediaType = 'video';
    hasMedia = true;
    text = content.videoMessage.caption || '[Video]';
  } else if (content.audioMessage) {
    mediaType = 'audio';
    hasMedia = true;
    text = '[Audio]';
  } else if (content.documentMessage) {
    mediaType = 'document';
    hasMedia = true;
    text = content.documentMessage.fileName || content.documentMessage.caption || '[Document]';
  } else if (content.protocolMessage || content.senderKeyDistributionMessage) {
    return null; // Ignore internal protocol messages
  } else {
    text = '[Message]';
  }

  return { text, mediaType, hasMedia };
}

function upsertContact(contact) {
  if (!contact || !contact.id) return;
  const existing = contacts.get(contact.id) || {};
  contacts.set(contact.id, {
    name: contact.name || existing.name,
    notify: contact.notify || existing.notify,
    phoneNumber: contact.phoneNumber || existing.phoneNumber,
    lid: contact.lid || existing.lid,
  });
}

function resolveName(jid, fallback) {
  const c = contacts.get(jid);
  return (c && (c.name || c.notify)) || fallback || jid;
}

function isSavedContact(jid) {
  if (jid.endsWith('@g.us')) return true; // Groups are treated as saved
  const c = contacts.get(jid);
  return Boolean(c && c.name && c.name.trim().length > 0);
}

function reResolveAllChatNames() {
  for (const [jid, chat] of chats.entries()) {
    const updatedName = resolveName(jid, chat.name);
    if (updatedName !== chat.name) {
      chat.name = updatedName;
      chats.set(jid, chat);
    }
  }
}

function upsertChat(chat, isInitialSync = false) {
  if (!chat || !chat.id) return;
  const existing = chats.get(chat.id) || {};
  const resolvedName = resolveName(chat.id, chat.name || chat.subject || existing.name);

  const unreadCount = (existing.unreadCount !== undefined && !isInitialSync)
    ? existing.unreadCount
    : (chat.unreadCount ?? existing.unreadCount ?? 0);

  const lastMessageTs = Math.max(
    parseTimestamp(chat.conversationTimestamp),
    parseTimestamp(existing.lastMessageTs)
  );

  chats.set(chat.id, {
    id: chat.id,
    name: resolvedName,
    unreadCount: unreadCount,
    lastMessageTs: lastMessageTs,
  });
}

function recordMessage(msg, isHistorySync = false) {
  const jid = msg.key?.remoteJid;
  if (!jid) return;

  if (msg.pushName && !msg.key.fromMe) {
    upsertContact({ id: jid, notify: msg.pushName });
  }

  const parsed = parseMessageContent(msg);
  if (!parsed) return;

  const list = messages.get(jid) || [];
  const existingIdx = list.findIndex((m) => m.id === msg.key.id);

  const timestamp = parseTimestamp(msg.messageTimestamp);
  const msgObj = {
    id: msg.key.id,
    fromMe: Boolean(msg.key.fromMe),
    timestamp: timestamp,
    text: parsed.text,
    mediaType: parsed.mediaType,
    hasMedia: parsed.hasMedia,
    mediaUrl: parsed.hasMedia
      ? `/messages/${encodeURIComponent(jid)}/${encodeURIComponent(msg.key.id)}/media`
      : null,
    rawMsg: parsed.hasMedia ? msg : null,
  };

  // Overwrite if updating placeholder/media message, otherwise push
  if (existingIdx !== -1) {
    const prev = list[existingIdx];
    if (!prev.hasMedia && parsed.hasMedia) {
      list[existingIdx] = msgObj;
    }
  } else {
    list.push(msgObj);
  }

  list.sort((a, b) => a.timestamp - b.timestamp);

  if (list.length > MAX_MESSAGES_PER_CHAT) {
    list.shift();
  }
  messages.set(jid, list);

  const existingChat = chats.get(jid) || { id: jid, name: resolveName(jid), unreadCount: 0, lastMessageTs: 0 };
  existingChat.name = resolveName(jid, existingChat.name);
  existingChat.lastMessageTs = Math.max(existingChat.lastMessageTs || 0, timestamp);

  if (!msg.key.fromMe && !isHistorySync) {
    existingChat.unreadCount = (existingChat.unreadCount || 0) + 1;
  }

  chats.set(jid, existingChat);
}

// On-Demand History Sync for chats with few messages loaded
async function triggerOnDemandHistorySync(jid) {
  if (!sock || connectionStatus !== 'connected') return;
  if (pendingHistoryRequests.has(jid)) return;

  const list = messages.get(jid) || [];
  if (list.length === 0) return;

  const oldest = list[0];
  if (!oldest || !oldest.rawMsg || !oldest.rawMsg.key) return;

  try {
    pendingHistoryRequests.add(jid);
    console.log(`[History Sync] Requesting 50 older messages on-demand for ${jid}...`);
    await sock.fetchMessageHistory(50, oldest.rawMsg.key, oldest.timestamp * 1000);
  } catch (err) {
    console.log(`[History Sync] On-demand history fetch failed for ${jid}: ${err.message}`);
  } finally {
    setTimeout(() => pendingHistoryRequests.delete(jid), 15000);
  }
}

async function startSock() {
  const { state, saveCreds } = await useMultiFileAuthState(AUTH_DIR);
  const { version } = await fetchLatestBaileysVersion();

  sock = makeWASocket({
    auth: state,
    version,
    logger: pino({ level: 'silent' }),
    printQRInTerminal: false,
    syncFullHistory: true,
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', (update) => {
    const { connection, lastDisconnect, qr } = update;
    if (qr) {
      latestQR = qr;
      connectionStatus = 'qr';
      console.log('[Baileys] New QR issued — fetch PNG from GET /qr-image');
    }
    if (connection === 'open') {
      connectionStatus = 'connected';
      latestQR = null;
      console.log('[Baileys] WhatsApp connection open');
    }
    if (connection === 'close') {
      const statusCode = lastDisconnect?.error?.output?.statusCode;
      const loggedOut = statusCode === DisconnectReason.loggedOut;
      connectionStatus = loggedOut ? 'loggedOut' : 'disconnected';
      console.log(`[Baileys] Connection closed. statusCode=${statusCode}, loggedOut=${loggedOut}`);
      if (!loggedOut) {
        setTimeout(() => startSock(), 3000);
      }
    }
  });

  sock.ev.on('messaging-history.set', ({ chats: syncedChats, contacts: syncedContacts, messages: syncedMessages }) => {
    (syncedContacts || []).forEach(upsertContact);
    (syncedChats || []).forEach((c) => upsertChat(c, true));

    if (Array.isArray(syncedMessages) && syncedMessages.length > 0) {
      const sorted = [...syncedMessages].sort((a, b) => parseTimestamp(a.messageTimestamp) - parseTimestamp(b.messageTimestamp));
      sorted.forEach((msg) => recordMessage(msg, true));
    }
    reResolveAllChatNames();
  });

  sock.ev.on('contacts.upsert', (newContacts) => {
    (newContacts || []).forEach(upsertContact);
    reResolveAllChatNames();
  });

  sock.ev.on('contacts.update', (updates) => {
    (updates || []).forEach((u) => upsertContact({ id: u.id, ...u }));
    reResolveAllChatNames();
  });

  sock.ev.on('chats.upsert', (newChats) => {
    (newChats || []).forEach((c) => upsertChat(c, false));
  });

  sock.ev.on('chats.update', (updates) => {
    (updates || []).forEach((u) => upsertChat({ id: u.id, ...u }, false));
  });

  sock.ev.on('messages.upsert', ({ messages: msgs }) => {
    (msgs || []).forEach((m) => recordMessage(m, false));
  });

  sock.ev.on('messages.update', (updates) => {
    for (const item of updates) {
      if (item.key && item.update) {
        // Re-record if media was decrypted
      }
    }
  });
}

// ---------- Express Middleware & Routes ----------
app.use(express.json({ limit: '16mb' }));
app.use(express.urlencoded({ extended: true, limit: '16mb' }));

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, Authorization');
  if (req.method === 'OPTIONS') {
    return res.sendStatus(200);
  }
  next();
});

app.get('/health', (req, res) => {
  res.type('application/json').send(safeJSONStringify({
    status: connectionStatus,
    user: sock?.user?.id || null,
    totalChats: chats.size,
    totalContacts: contacts.size,
  }));
});

app.get('/qr', async (req, res) => {
  if (connectionStatus === 'connected') {
    return res.type('application/json').send(safeJSONStringify({ status: 'connected', qr: null }));
  }
  if (!latestQR) {
    return res.status(404).type('application/json').send(safeJSONStringify({ status: connectionStatus, error: 'No QR available yet' }));
  }
  const dataUrl = await QRCode.toDataURL(latestQR);
  res.type('application/json').send(safeJSONStringify({ status: 'qr', qr: dataUrl }));
});

app.get('/qr-image', (req, res) => {
  if (connectionStatus === 'connected') {
    return res.status(404).send('Already connected — no QR to show.');
  }
  if (!latestQR) {
    return res.status(404).send('No QR yet — refresh in a few seconds.');
  }
  let size = parseInt(req.query.size, 10);
  if (!size || size < 80 || size > 500) size = 180;

  res.type('png');
  QRCode.toFileStream(res, latestQR, { width: size });
});

// List All Chats (ASCII-safe JSON serializer, filters @lid and unsaved numbers)
app.get('/chats', (req, res) => {
  if (connectionStatus !== 'connected') {
    return res.status(503).type('application/json').send(safeJSONStringify({ error: `Not connected (status: ${connectionStatus})` }));
  }

  const allChats = Array.from(chats.values());
  const filtered = allChats.filter((chat) => {
    if (!chat || !chat.id) return false;
    if (chat.id.endsWith('@lid')) return false;
    return isSavedContact(chat.id);
  });

  filtered.sort((a, b) => (b.lastMessageTs || 0) - (a.lastMessageTs || 0));
  res.type('application/json').send(safeJSONStringify(filtered));
});

// Get Messages for a Chat (ASCII-safe JSON serializer)
app.get('/messages/:jid', (req, res) => {
  const jid = decodeURIComponent(req.params.jid);
  const chat = chats.get(jid);
  if (chat) {
    chat.unreadCount = 0;
    chats.set(jid, chat);
  }

  const list = messages.get(jid) || [];
  if (list.length > 0 && list.length <= 10) {
    triggerOnDemandHistorySync(jid);
  }

  const cleanList = list.map(({ rawMsg, ...rest }) => rest);
  res.type('application/json').send(safeJSONStringify(cleanList));
});

// Profile Picture Endpoint
app.get('/profile-pic/:jid', async (req, res) => {
  if (!sock || connectionStatus !== 'connected') {
    return res.status(503).send('Not connected');
  }
  const jid = decodeURIComponent(req.params.jid);
  try {
    const url = await sock.profilePictureUrl(jid, 'image');
    if (!url) return res.status(404).send('No profile picture available');
    const response = await fetch(url);
    if (!response.ok) return res.status(404).send('Failed to download profile picture');
    const arrayBuf = await response.arrayBuffer();
    res.type('image/jpeg');
    return res.send(Buffer.from(arrayBuf));
  } catch (err) {
    res.status(404).send('Profile picture unavailable');
  }
});

// Media Download Endpoint with jpegThumbnail fallback for historical/expired photos
app.get('/messages/:jid/:msgId/media', async (req, res) => {
  if (!sock || connectionStatus !== 'connected') {
    return res.status(503).send('Not connected to WhatsApp');
  }
  const jid = decodeURIComponent(req.params.jid);
  const msgId = decodeURIComponent(req.params.msgId);

  const list = messages.get(jid) || [];
  const msgObj = list.find((m) => m.id === msgId);
  if (!msgObj || !msgObj.rawMsg) {
    return res.status(404).send('Media message not found or raw payload evicted');
  }

  try {
    const buffer = await downloadMediaMessage(
      msgObj.rawMsg,
      'buffer',
      {},
      {
        logger: pino({ level: 'silent' }),
        reuploadRequest: sock.updateMediaMessage,
      }
    );

    if (msgObj.mediaType === 'image') {
      res.type('image/jpeg');
    } else if (msgObj.mediaType === 'video') {
      res.type('video/mp4');
    } else if (msgObj.mediaType === 'audio') {
      res.type('audio/ogg');
    } else {
      res.type('application/octet-stream');
    }
    return res.send(buffer);
  } catch (err) {
    console.warn(`[Media Download Warning] High-res download failed (${err.message}). Using jpegThumbnail fallback...`);
    const content = unwrapMessage(msgObj.rawMsg?.message);
    const thumb = content?.imageMessage?.jpegThumbnail || content?.videoMessage?.jpegThumbnail;
    if (thumb) {
      const buffer = Buffer.from(thumb);
      res.type('image/jpeg');
      return res.send(buffer);
    }
    res.status(404).send('Media unavailable or expired');
  }
});

// Send Message (supports JSON with text OR base64 media for J2ME CLDC/MIDP)
// IMMEDIATELY RECORDS SENT MESSAGE IN BACKEND STORE
app.post('/send', async (req, res) => {
  const { jid, text, caption, mediaType, mediaBase64 } = req.body || {};
  if (!sock || connectionStatus !== 'connected') {
    return res.status(503).json({ error: `Not connected (status: ${connectionStatus})` });
  }
  if (!jid) {
    return res.status(400).json({ error: 'jid is required' });
  }

  try {
    let result;
    if (mediaBase64 && mediaType) {
      const buffer = Buffer.from(mediaBase64, 'base64');
      if (mediaType === 'image') {
        result = await sock.sendMessage(jid, { image: buffer, caption: caption || text || '' });
      } else if (mediaType === 'video') {
        result = await sock.sendMessage(jid, { video: buffer, caption: caption || text || '' });
      } else if (mediaType === 'audio') {
        result = await sock.sendMessage(jid, { audio: buffer, mimetype: 'audio/mp4' });
      } else {
        return res.status(400).json({ error: 'Unsupported mediaType. Use image, video, or audio.' });
      }
    } else if (text) {
      result = await sock.sendMessage(jid, { text });
    } else {
      return res.status(400).json({ error: 'Either text or mediaBase64+mediaType is required' });
    }

    if (result) {
      recordMessage(result, false);
    }
    res.type('application/json').send(safeJSONStringify({ ok: true, id: result?.key?.id }));
  } catch (err) {
    console.error('[Send Message Error]:', err);
    res.status(500).type('application/json').send(safeJSONStringify({ error: err.message }));
  }
});

// Send Media (Multipart Form-Data for binary file upload)
const upload = multer({ limits: { fileSize: 16 * 1024 * 1024 } });
app.post('/send-media', upload.single('file'), async (req, res) => {
  if (!sock || connectionStatus !== 'connected') {
    return res.status(503).json({ error: 'Not connected to WhatsApp' });
  }
  const { jid, caption, mediaType } = req.body || {};
  const file = req.file;
  if (!jid || !file) {
    return res.status(400).json({ error: 'jid and file are required' });
  }
  try {
    const type = mediaType || (file.mimetype.startsWith('video') ? 'video' : file.mimetype.startsWith('audio') ? 'audio' : 'image');
    let payload = {};
    if (type === 'image') {
      payload = { image: file.buffer, caption: caption || '' };
    } else if (type === 'video') {
      payload = { video: file.buffer, caption: caption || '' };
    } else if (type === 'audio') {
      payload = { audio: file.buffer, mimetype: file.mimetype || 'audio/mp4' };
    } else {
      payload = { document: file.buffer, fileName: file.originalname, mimetype: file.mimetype };
    }
    const result = await sock.sendMessage(jid, payload);
    if (result) {
      recordMessage(result, false);
    }
    res.type('application/json').send(safeJSONStringify({ ok: true, id: result?.key?.id }));
  } catch (err) {
    console.error('[Send Media Error]:', err);
    res.status(500).type('application/json').send(safeJSONStringify({ error: err.message }));
  }
});

// ---------- Boot ----------
startSock().then(() => {
  app.listen(PORT, () => {
    console.log(`[Server] WhatsApp Baileys backend listening on port ${PORT}`);
  });
}).catch((err) => {
  console.error('[Server Boot Error]:', err);
});
