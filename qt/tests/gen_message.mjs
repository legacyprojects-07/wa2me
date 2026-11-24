// Generates the pre-key bundle response used by qt/tests/test_message.cpp.
//
// It plays the role of the WhatsApp server answering
//   <iq type=get xmlns=encrypt><key><user jid="5511@s.whatsapp.net"/></key></iq>
// with Bob's device bundle, encoded with Baileys' own BinaryNode encoder.
// Requires BAILEYS_DIR.
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const require = createRequire(BAILEYS_DIR + '/package.json');
const libsignal = require('libsignal');
const { encodeBinaryNode } = await import(BAILEYS_DIR + '/lib/WABinary/encode.js');

const KeyHelper = libsignal.keyhelper;
const h = b => Buffer.from(b).toString('hex');
const be = (n, len) => {
  const out = Buffer.alloc(len);
  for (let i = len - 1; i >= 0; i--) { out[i] = n & 0xff; n >>= 8; }
  return out;
};

// ── Bob (the peer device) ─────────────────────────────────────────────────
const bobIdentity = KeyHelper.generateIdentityKeyPair();
const bobRegId = KeyHelper.generateRegistrationId(false);
const bobSignedPreKey = KeyHelper.generateSignedPreKey(bobIdentity, 5);
const bobPreKey = KeyHelper.generatePreKey(31337);

const bundleNode = {
  tag: 'iq',
  attrs: { id: 'KEYREQ1', type: 'result', xmlns: 'encrypt', to: 's.whatsapp.net' },
  content: [{
    tag: 'list',
    attrs: {},
    content: [{
      tag: 'user',
      attrs: { jid: '5511@s.whatsapp.net' },
      content: [{
        tag: 'device',
        attrs: { jid: '5511:0@s.whatsapp.net' },
        content: [
          {
            tag: 'key',
            attrs: {},
            content: [
              { tag: 'id', attrs: {}, content: be(bobPreKey.keyId, 3) },
              { tag: 'value', attrs: {}, content: bobPreKey.keyPair.pubKey },
            ],
          },
          {
            tag: 'skey',
            attrs: {},
            content: [
              { tag: 'id', attrs: {}, content: be(bobSignedPreKey.keyId, 3) },
              { tag: 'value', attrs: {}, content: bobSignedPreKey.keyPair.pubKey },
              { tag: 'signature', attrs: {}, content: bobSignedPreKey.signature },
            ],
          },
          { tag: 'identity', attrs: {}, content: bobIdentity.pubKey },
          { tag: 'registration', attrs: {}, content: be(bobRegId, 4) },
        ],
      }],
    }],
  }],
};

const frame = encodeBinaryNode(bundleNode);
const lines = [
  `BOBKEY ${h(bobIdentity.privKey)} ${h(bobIdentity.pubKey)} ${bobRegId}`,
  `BOBSIGNED ${bobSignedPreKey.keyId} ${h(bobSignedPreKey.keyPair.privKey)} ${h(bobSignedPreKey.keyPair.pubKey)} ${h(bobSignedPreKey.signature)}`,
  `BOBPREKEY ${bobPreKey.keyId} ${h(bobPreKey.keyPair.privKey)} ${h(bobPreKey.keyPair.pubKey)}`,
  `KEYRESP ${h(frame)}`,
];
fs.writeFileSync('/home/user/wa2me/qt/tests/message_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines; bundle frame', frame.length, 'bytes');
