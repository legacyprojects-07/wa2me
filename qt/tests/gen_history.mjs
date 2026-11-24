// Generates media/history vectors for qt/tests/test_history.cpp using Baileys'
// own crypto (HKDF + AES-CBC + HMAC) and protobuf encoders.
// Requires BAILEYS_DIR (the @whiskeysockets/baileys package directory).
import fs from 'fs';
import zlib from 'zlib';
import Crypto from 'crypto';
import { pathToFileURL } from 'url';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const { proto } = await import(pathToFileURL(BAILEYS_DIR + '/WAProto/index.js').href);

const h = b => Buffer.from(b).toString('hex');

const hkdf = (key, length, { salt = Buffer.alloc(0), info = Buffer.alloc(0) } = {}) =>
  Buffer.from(Crypto.hkdfSync('sha256', key, salt, info, length));

const mediaKeys = (mediaKey, type) => {
  const expanded = hkdf(mediaKey, 112, { info: Buffer.from(`WhatsApp ${type} Keys`) });
  return {
    iv: expanded.slice(0, 16),
    cipherKey: expanded.slice(16, 48),
    macKey: expanded.slice(48, 80),
  };
};

// media key name -> the HKDF mapping used by MEDIA_HKDF_KEY_MAPPING
const MAPPING = {
  image: 'Image', video: 'Video', audio: 'Audio', document: 'Document',
  'md-msg-hist': 'History', 'md-app-state': 'App State',
  'thumbnail-image': 'Image Thumbnail', 'thumbnail-video': 'Video Thumbnail',
  'thumbnail-document': 'Document Thumbnail', 'thumbnail-link': 'Link Thumbnail',
  'sticker': 'Image', 'ptt': 'Audio',
};

// WhatsApp media format: AES-256-CBC(pkcs7) || hmac(macKey, iv||ct)[0..10]
const encryptMedia = (plaintext, mediaKey, type) => {
  const { iv, cipherKey, macKey } = mediaKeys(mediaKey, MAPPING[type]);
  const cipher = Crypto.createCipheriv('aes-256-cbc', cipherKey, iv);
  const ciphertext = Buffer.concat([cipher.update(plaintext), cipher.final()]);
  const mac = Crypto.createHmac('sha256', macKey).update(Buffer.concat([iv, ciphertext])).digest().slice(0, 10);
  return Buffer.concat([ciphertext, mac]);
};

const lines = [];

// 1. HistorySync blob: exactly what downloadContentFromMessage(msg,'md-msg-hist') gives us.
const historyObj = {
  syncType: 3, // RECENT
  progress: 66,
  chunkOrder: 1,
  conversations: [
    {
      id: '919999999999@s.whatsapp.net',
      unreadCount: 2,
      conversationTimestamp: 1700000700,
      messages: [
        {
          message: {
            key: { remoteJid: '919999999999@s.whatsapp.net', fromMe: false, id: 'HIST1' },
            message: { conversation: 'older message' },
            messageTimestamp: 1700000600,
          },
        },
        {
          message: {
            key: { remoteJid: '919999999999@s.whatsapp.net', fromMe: true, id: 'HIST2' },
            message: { extendedTextMessage: { text: 'newer reply' } },
            messageTimestamp: 1700000700,
          },
        },
      ],
    },
    {
      id: '120363041234567890@g.us',
      name: 'Family',
      unreadCount: 1,
      conversationTimestamp: 1700000800,
      messages: [
        {
          message: {
            key: {
              remoteJid: '120363041234567890@g.us', fromMe: false, id: 'HIST3',
              participant: '918888888888@s.whatsapp.net',
            },
            message: { conversation: 'group hello' },
            messageTimestamp: 1700000800,
          },
        },
      ],
    },
  ],
};
const historyProto = proto.HistorySync.encode(proto.HistorySync.fromObject(historyObj)).finish();
const mediaKey = Buffer.alloc(32, 0x5a);
const compressed = zlib.deflateSync(historyProto);
const historyBlob = encryptMedia(compressed, mediaKey, 'md-msg-hist');
lines.push(`HISTORY ${h(historyBlob)} ${h(mediaKey)} 3 66 1 2 3`);
lines.push(`HISTORY_TEXT ${h(historyProto)} 3 66 1 2 3`);

// 2. A pushname chunk merged into the same client state.
const pushObj = {
  syncType: 4, // PUSH_NAME
  progress: 100,
  pushnames: [
    { id: '919999999999@s.whatsapp.net', pushname: 'Me' },
    { id: '918888888888@s.whatsapp.net', pushname: 'Ravi' },
  ],
};
const pushProto = proto.HistorySync.encode(proto.HistorySync.fromObject(pushObj)).finish();
const pushBlob = encryptMedia(zlib.deflateSync(pushProto), mediaKey, 'md-msg-hist');
lines.push(`PUSHNAMES ${h(pushBlob)} ${h(mediaKey)} 4 100`);

// 3. Generic media blob (an image), to prove the shared media crypto works.
const imageBytes = Buffer.from('a jpeg would be here, but bytes are bytes');
const imageKey = Buffer.alloc(32, 0x11);
const imageBlob = encryptMedia(imageBytes, imageKey, 'image');
lines.push(`MEDIA image ${h(imageBlob)} ${h(imageKey)} ${h(imageBytes)}`);
lines.push(`MEDIA thumbnail-image ${h(encryptMedia(Buffer.from('thumb'), imageKey, 'thumbnail-image'))} ${h(imageKey)} ${h(Buffer.from('thumb'))}`);
lines.push(`MEDIA md-app-state ${h(encryptMedia(Buffer.from('appstate'), imageKey, 'md-app-state'))} ${h(imageKey)} ${h(Buffer.from('appstate'))}`);

// 4. A trivially corrupt blob: flip a ciphertext byte so the MAC must fail.
const corrupt = Buffer.from(imageBlob);
corrupt[3] ^= 0xff;
lines.push(`CORRUPT ${h(corrupt)} ${h(imageKey)}`);

fs.writeFileSync('/home/user/wa2me/qt/tests/history_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
