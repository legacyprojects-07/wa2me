// Vector generator for the Qt-free core tests.
//
// Requires the real Baileys package (with its deps) and Node >= 18:
//   BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys \
//     node qt/tests/gen_noise.mjs.mjs
//
// The checked-in *_vectors.txt files were generated this way against
// @whiskeysockets/baileys 6.7.22, so the C++ port is verified against the real
// reference implementation rather than hand-copied constants.
import crypto from 'crypto';
import fs from 'fs';

import { pathToFileURL } from 'url';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const baileysImport = p => pathToFileURL(BAILEYS_DIR + p).href;

const { makeNoiseHandler } = await import(baileysImport('/lib/Utils/noise-handler.js'));
const NOISE_HEADER = Buffer.from([87, 65, 6, 3]);

const logger = { child: () => logger, trace: () => {}, info: () => {}, debug: () => {}, error: () => {} };

// Deterministic keypair (32-byte priv + derived pub via node x25519).
function keyPairFromPriv(privHex) {
  const priv = Buffer.from(privHex, 'hex');
  const ecdh = crypto.createPrivateKey({
    key: Buffer.concat([Buffer.from('302e020100300506032b656e04220420', 'hex'), priv]),
    format: 'der', type: 'pkcs8',
  });
  const pubDer = crypto.createPublicKey(ecdh).export({ format: 'der', type: 'spki' });
  const pub = pubDer.subarray(pubDer.length - 32);
  return { private: priv, public: pub };
}

const privHex = crypto.randomBytes(32).toString('hex');
const kp = keyPairFromPriv(privHex);

const noise = makeNoiseHandler({
  keyPair: { private: kp.private, public: kp.public },
  NOISE_HEADER,
  logger,
  routingInfo: undefined,
});

const lines = [];
lines.push(`NOISEKEY ${privHex} ${kp.public.toString('hex')}`);

const ops = [];
// pre-handshake: authenticate + encrypt ops exercise hash chaining + HKDF counters
ops.push(['A', crypto.randomBytes(5)]);
ops.push(['E', crypto.randomBytes(32)]);
ops.push(['A', crypto.randomBytes(33)]);
ops.push(['E', crypto.randomBytes(64)]);
ops.push(['A', Buffer.alloc(0)]);
ops.push(['E', crypto.randomBytes(17)]);
ops.push(['F', null]);                       // finishInit
ops.push(['E', crypto.randomBytes(48)]);     // post-init encrypt (new keys, counter 0)
ops.push(['E', crypto.randomBytes(1)]);
ops.push(['E', Buffer.alloc(0)]);
ops.push(['E', crypto.randomBytes(300)]);

for (const [op, data] of ops) {
  if (op === 'A') {
    noise.authenticate(data);
    lines.push(`OP A ${data.toString('hex') || '-'}`);
  } else if (op === 'E') {
    const out = noise.encrypt(data);
    lines.push(`OP E ${data.toString('hex') || '-'} ${out.toString('hex')}`);
  } else if (op === 'F') {
    await noise.finishInit();
    lines.push('OP F');
  }
}

fs.writeFileSync('/home/user/wa2me/qt/tests/noise_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
