require('dotenv').config();

const express = require('express');
const pino = require('pino');
const QRCode = require('qrcode');
const { MongoClient } = require('mongodb');
const {
  default: makeWASocket,
  DisconnectReason,
  fetchLatestBaileysVersion,
} = require('@whiskeysockets/baileys');

const { useMongoAuthState } = require('./mongoAuthState');

const app = express();
const PORT = process.env.PORT || 3000;

let sock = null;
let latestQR = null;
let connectionStatus = 'starting'; // starting | qr | connected | disconnected

// Baileys' built-in in-memory store was removed from core, so we track
// chats ourselves off the events it emits. Good enough for a J2ME client
// that just wants a chat list + recent messages, not full sync semantics.
const chats = new Map(); // jid -> { id, name, unreadCount, lastMessageTs }
const messages = new Map(); // jid -> array of recent messages (capped)
const MAX_MESSAGES_PER_CHAT = 50;

function upsertChat(chat) {
  const existing = chats.get(chat.id) || {};
  chats.set(chat.id, {
    id: chat.id,
    name: chat.name || chat.subject || existing.name || chat.id,
    unreadCount: chat.unreadCount ?? existing.unreadCount ?? 0,
    lastMessageTs: chat.conversationTimestamp ?? existing.lastMessageTs ?? 0,
  });
}

function recordMessage(msg) {
  const jid = msg.key?.remoteJid;
  if (!jid) return;
  const list = messages.get(jid) || [];
  list.push({
    id: msg.key.id,
    fromMe: msg.key.fromMe,
    timestamp: msg.messageTimestamp,
    text:
      msg.message?.conversation ||
      msg.message?.extendedTextMessage?.text ||
      (msg.message?.imageMessage ? '[image]' : null) ||
      (msg.message?.videoMessage ? '[video]' : null) ||
      (msg.message?.audioMessage ? '[audio]' : null) ||
      null,
  });
  if (list.length > MAX_MESSAGES_PER_CHAT) list.shift();
  messages.set(jid, list);

  // Bump chat's lastMessageTs so /chats can sort by recency
  const chat = chats.get(jid) || { id: jid, name: jid, unreadCount: 0 };
  chat.lastMessageTs = msg.messageTimestamp;
  chats.set(jid, chat);
}

async function startSock(collection) {
  const { state, saveCreds } = await useMongoAuthState(collection);
  const { version } = await fetchLatestBaileysVersion();

  sock = makeWASocket({
    auth: state,
    version,
    logger: pino({ level: 'silent' }),
    printQRInTerminal: false,
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', (update) => {
    const { connection, lastDisconnect, qr } = update;

    if (qr) {
      latestQR = qr;
      connectionStatus = 'qr';
      console.log('New QR issued — fetch it from GET /qr');
    }

    if (connection === 'open') {
      connectionStatus = 'connected';
      latestQR = null;
      console.log('WhatsApp connection open');
    }

    if (connection === 'close') {
      connectionStatus = 'disconnected';
      const statusCode = lastDisconnect?.error?.output?.statusCode;
      const loggedOut = statusCode === DisconnectReason.loggedOut;
      console.log('Connection closed. loggedOut =', loggedOut);
      if (!loggedOut) {
        startSock(collection); // auto-reconnect
      }
    }
  });

  // Initial history sync gives us the chat list on first login/reconnect
  sock.ev.on('messaging-history.set', ({ chats: syncedChats }) => {
    (syncedChats || []).forEach(upsertChat);
  });

  sock.ev.on('chats.upsert', (newChats) => {
    (newChats || []).forEach(upsertChat);
  });

  sock.ev.on('chats.update', (updates) => {
    (updates || []).forEach((u) => upsertChat({ id: u.id, ...u }));
  });

  sock.ev.on('messages.upsert', ({ messages: msgs }) => {
    (msgs || []).forEach(recordMessage);
  });
}

// ---------- Routes ----------

app.use(express.json());

app.get('/health', (req, res) => {
  res.json({ status: connectionStatus });
});

app.get('/qr', async (req, res) => {
  if (connectionStatus === 'connected') {
    return res.json({ status: 'connected', qr: null });
  }
  if (!latestQR) {
    return res.status(404).json({ status: connectionStatus, error: 'No QR available yet' });
  }
  const dataUrl = await QRCode.toDataURL(latestQR);
  res.json({ status: 'qr', qr: dataUrl });
});

app.get('/chats', (req, res) => {
  if (connectionStatus !== 'connected') {
    return res.status(503).json({ error: `Not connected (status: ${connectionStatus})` });
  }
  const list = Array.from(chats.values()).sort(
    (a, b) => (b.lastMessageTs || 0) - (a.lastMessageTs || 0)
  );
  res.json(list);
});

app.get('/messages/:jid', (req, res) => {
  const jid = decodeURIComponent(req.params.jid);
  res.json(messages.get(jid) || []);
});

app.post('/send', async (req, res) => {
  const { jid, text } = req.body || {};
  if (!sock || connectionStatus !== 'connected') {
    return res.status(503).json({ error: 'Not connected' });
  }
  if (!jid || !text) {
    return res.status(400).json({ error: 'jid and text are required' });
  }
  try {
    await sock.sendMessage(jid, { text });
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ---------- Boot ----------

(async () => {
  if (!process.env.MONGO_URI) {
    console.error('MONGO_URI env var is required');
    process.exit(1);
  }
  const mongoClient = new MongoClient(process.env.MONGO_URI);
  await mongoClient.connect();
  const collection = mongoClient.db('whatsapp_baileys').collection('auth');

  await startSock(collection);

  app.listen(PORT, () => console.log(`Server listening on port ${PORT}`));
})();
