// Phase 2 of qt/tests/test_media.cpp: the receive side of media, in Baileys.
//
//   * proto.Message decodes the ImageMessage/DocumentMessage we built
//   * Baileys' media download path (getMediaKeys + AES-CBC + HMAC check)
//     decrypts the blob we produced
//   * a document upload is encrypted for the C++ side to decrypt
import fs from 'fs';
import crypto from 'crypto';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const [, , vectorsPath, dir] = process.argv;
const h = b => Buffer.from(b).toString('hex');

await import(BAILEYS_DIR + '/lib/index.js');
const media = await import(BAILEYS_DIR + '/lib/Utils/messages-media.js');
const { proto } = await import(BAILEYS_DIR + '/WAProto/index.js');

const cpp = {};
for (const line of fs.readFileSync(dir + '/cpp_media.txt', 'utf8').split('\n').filter(Boolean)) {
  const [kind, value] = line.split(' ');
  cpp[kind] = value;
}

let failures = 0;
const check = (ok, what, extra = '') => {
  if (ok) { console.log('ok   ' + what); return; }
  failures++;
  console.error('FAIL ' + what + (extra ? ' — ' + extra : ''));
};

// ── 1. Decode what we sent ────────────────────────────────────────────────
const ours = proto.Message.decode(Buffer.from(cpp.CPPMEDIAMSG, 'hex'));
check(!!ours.imageMessage, 'our message decodes as an image message');
check(ours.imageMessage?.caption === 'sent from qt', 'caption',
      ours.imageMessage?.caption);
check(ours.imageMessage?.mimetype === 'image/jpeg', 'mimetype', ours.imageMessage?.mimetype);
check(ours.imageMessage?.mediaKey?.length === 32, 'media key is 32 bytes');
check(ours.imageMessage?.fileEncSha256?.length === 32, 'encrypted hash present');
check(ours.imageMessage?.directPath === '/mms/image/OURS', 'direct path');
check(ours.imageMessage?.height === 120 && ours.imageMessage?.width === 160, 'dimensions');

const ourDoc = proto.Message.decode(Buffer.from(cpp.CPPMEDIADOC, 'hex'));
check(ourDoc.documentMessage?.fileName === 'notes.pdf', 'document file name',
      ourDoc.documentMessage?.fileName);
check(ourDoc.documentMessage?.title === 'notes.pdf', 'document title');
check(ourDoc.documentMessage?.mimetype === 'application/pdf', 'document mimetype');

// ── 2. Download path: decrypt the blob the C++ side produced ──────────────
const mediaKey = Buffer.from(cpp.CPPMEDIAKEY, 'hex');
const blob = Buffer.from(cpp.CPPMEDIABLOB, 'hex');
const plaintext = Buffer.from(cpp.CPPMEDIAPLAIN, 'hex');
const keys = await media.getMediaKeys(mediaKey, 'image');

const ciphertext = blob.subarray(0, blob.length - 10);
const mac = blob.subarray(blob.length - 10);
const expectedMac = crypto.createHmac('sha256', keys.macKey)
  .update(Buffer.concat([keys.iv, ciphertext])).digest().subarray(0, 10);
check(expectedMac.equals(mac), 'our MAC verifies with Baileys-derived keys');

const decipher = crypto.createDecipheriv('aes-256-cbc', keys.cipherKey, keys.iv);
const decrypted = Buffer.concat([decipher.update(ciphertext), decipher.final()]);
check(h(decrypted) === h(plaintext), 'Baileys decrypted our blob');

// ── 3. A document upload for the C++ side ─────────────────────────────────
const docPlain = Buffer.alloc(777);
for (let i = 0; i < docPlain.length; i++) docPlain[i] = (i * 7) & 0xff;
const docKey = Buffer.alloc(32, 0x5a);
const docEnc = await media.encryptedStream(docPlain, 'document');
// encryptedStream() does not await its write stream's 'finish'.
for (let i = 0; i < 100; i++) {
  if (fs.existsSync(docEnc.encFilePath) &&
      fs.statSync(docEnc.encFilePath).size === docPlain.length + 10) break;
  await new Promise(r => setTimeout(r, 20));
}

fs.writeFileSync(dir + '/node_media.txt',
  `NODEDECRYPTED ${h(decrypted)}\n` +
  `NODEMEDIAKEY ${h(docEnc.mediaKey)}\n` +
  `NODEMEDIABLOB ${h(fs.readFileSync(docEnc.encFilePath))}\n` +
  `NODEMEDIAPLAIN ${h(docPlain)}\n` +
  `NODEDOC ${h(proto.Message.encode({
    documentMessage: {
      url: 'https://mmg.whatsapp.net/mms/document/reply',
      mimetype: 'application/pdf',
      title: 'reply.pdf',
      fileName: 'reply.pdf',
      fileSha256: docEnc.fileSha256,
      fileLength: docPlain.length,
      mediaKey: docEnc.mediaKey,
      fileEncSha256: docEnc.fileEncSha256,
      directPath: '/mms/document/reply',
      mediaKeyTimestamp: 1700000123,
    },
  }).finish())}\n`);

console.log('');
console.log(failures === 0 ? 'MEDIA INTEROP (node side) OK' : `MEDIA INTEROP FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
