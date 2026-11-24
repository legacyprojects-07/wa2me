// Media (upload/download) vectors from Baileys' own helpers.
//
// Baileys' encryptedStream() is exactly the code path a real WhatsApp Web
// client uses to upload a file: random 32-byte mediaKey, HKDF("WhatsApp Image
// Keys"), AES-256-CBC, 10-byte truncated HMAC. We reuse it so the C++ port can
// be checked byte-for-byte (and the CDN path/hash, which is what the upload URL
// is keyed by).
//
// Usage: BAILEYS_DIR=... node qt/tests/gen_media.mjs
import fs from 'fs';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const h = b => Buffer.from(b).toString('hex');

await import(BAILEYS_DIR + '/lib/index.js');
const media = await import(BAILEYS_DIR + '/lib/Utils/messages-media.js');
const { proto } = await import(BAILEYS_DIR + '/WAProto/index.js');
const { encodeBinaryNode } = await import(BAILEYS_DIR + '/lib/WABinary/encode.js');

// Deterministic "image": every byte value, repeated, so padding and MAC
// truncation are both exercised.
const plaintext = Buffer.alloc(4096);
for (let i = 0; i < plaintext.length; i++) plaintext[i] = i & 0xff;
// encryptedStream() also takes a Buffer, which keeps the vector deterministic.
const enc = await media.encryptedStream(plaintext, 'image');
// encryptedStream() does not await its write stream's 'finish' event.
for (let i = 0; i < 100; i++) {
  if (fs.existsSync(enc.encFilePath) &&
      fs.statSync(enc.encFilePath).size === enc.fileLength + 10) break;
  await new Promise(r => setTimeout(r, 20));
}
const blob = fs.readFileSync(enc.encFilePath);
const uploadPath = media.encodeBase64EncodedStringForUpload(enc.fileEncSha256.toString('base64'));

// A media_conn reply, encoded with Baileys' own node encoder so the C++ parser
// is checked against the real wire format.
const mediaConnNode = {
  tag: 'iq',
  attrs: { id: 'MEDIAIQ1', type: 'result', to: 's.whatsapp.net' },
  content: [{
    tag: 'media_conn',
    attrs: { auth: 'AUTH-TOKEN', ttl: '3600' },
    content: [{ tag: 'host', attrs: { hostname: 'mmg.whatsapp.net' } }],
  }],
};
const mediaConnHex = h(encodeBinaryNode(mediaConnNode));

// A real ImageMessage produced by Baileys' protobuf encoder: the C++ decoder
// must read back the same fields.
const imageMessage = {
  url: 'https://mmg.whatsapp.net/mms/image/abc',
  mimetype: 'image/jpeg',
  caption: 'vector caption',
  fileSha256: Buffer.alloc(32, 0x11),
  fileLength: 4096,
  height: 480,
  width: 640,
  mediaKey: Buffer.alloc(32, 0x22),
  fileEncSha256: Buffer.alloc(32, 0x33),
  directPath: '/mms/image/xyz',
  mediaKeyTimestamp: 1700000000,
};
const imageMsgHex = h(proto.Message.encode({ imageMessage }).finish());

// The same for a document, which has a completely different field layout.
const documentMessage = {
  url: 'https://mmg.whatsapp.net/mms/document/abc',
  mimetype: 'application/pdf',
  title: 'vector.pdf',
  fileSha256: Buffer.alloc(32, 0x44),
  fileLength: 1234,
  pageCount: 2,
  mediaKey: Buffer.alloc(32, 0x55),
  fileName: 'vector.pdf',
  fileEncSha256: Buffer.alloc(32, 0x66),
  directPath: '/mms/document/xyz',
  mediaKeyTimestamp: 1700000001,
  caption: 'doc caption',
};
const documentMsgHex = h(proto.Message.encode({ documentMessage }).finish());

// Download URL forms: Baileys only trusts mmg.whatsapp.net URLs; anything else
// falls back to https://mmg.whatsapp.net + directPath.
const lines = [
  `MEDIAFILE ${h(plaintext)}`,
  `MEDIAKEY ${h(enc.mediaKey)}`,
  `MEDIABLOB ${h(blob)}`,
  `MEDIAHASH ${h(enc.fileSha256)} ${h(enc.fileEncSha256)}`,
  `MEDIAPATH ${uploadPath}`,
  `MEDIACONN ${mediaConnHex}`,
  `IMAGEBOX ${imageMsgHex}`,
  `DOCBOX ${documentMsgHex}`,
  `MEDIADOWNLOAD https://mmg.whatsapp.net/mms/image/xyz /mms/image/xyz`,
  `MEDIADOWNLOAD https://evil.example/mms/image/xyz /mms/image/xyz`,
];
fs.writeFileSync('/home/user/wa2me/qt/tests/media_vectors.txt', lines.join('\n') + '\n');
fs.unlinkSync(enc.encFilePath);
console.log('wrote media_vectors.txt');
