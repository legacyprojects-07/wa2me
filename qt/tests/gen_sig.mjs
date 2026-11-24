// Generates SHA-512 and XEdDSA vectors for qt/tests/test_ed25519.cpp.
//
// SHA-512 comes from Node's crypto module; the signatures come from
// curve25519-js, which is what libsignal (and therefore Baileys) uses. That
// pins the WhatsApp variant: the clamped X25519 private key is used directly as
// the Ed25519 scalar, and the public key's sign bit moves into bit 7 of the
// last signature byte.
//
// Requires BAILEYS_DIR so curve25519-js resolves from Baileys' dependency tree.
import crypto from 'crypto';
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const curve = createRequire(BAILEYS_DIR + '/package.json')('curve25519-js');
const h = b => Buffer.from(b).toString('hex');

const lines = [];

// ── SHA-512 (raw byte vectors) ────────────────────────────────────────────
const shaInputs = ['', 'abc', 'hello world', 'a'.repeat(200)];
for (const text of shaInputs) {
  const msg = Buffer.from(text, 'utf8');
  lines.push(`SHA512 ${msg.length ? h(msg) : '-'} ${crypto.createHash('sha512').update(msg).digest('hex')}`);
}

// ── XEdDSA ────────────────────────────────────────────────────────────────
// libsignal's curve.calculateSignature(privKey, message) — message lengths
// cover both the 32-byte X25519 form and the 33-byte 0x05-prefixed form used
// for signed pre-keys.
const cases = [
  ['0b2b1f70ef183269eb3e6a25ef5a0382b1234031328cfa0c3089cf2bf0382ce0', '4bfa8a291e1141b37aa111c1ef8e668e'],
  ['823575c0816254edf8294f7a076a7bed0c4d7544f175336183197abaed364dd3', '086cc19c80e6b90e550f9735dd1e199a3577b67598ecc8'],
  ['8d1635daa91cb2857036e98979f26489173cdf202f988a468c19118d26645fab', ''],
  ['39d7d3d7af207b54666fe364e77e005446fbc5b381f34abc5a30875537586751', '05' + 'aa'.repeat(32)],
  ['121b8e375cb6d3a7309a13b5ef862c421b71caf6a2084b03ab0e30da3db9ae1b', '00'],
];

for (const [privHex, msgHex] of cases) {
  const priv = Buffer.from(privHex, 'hex');
  const msg = msgHex ? Buffer.from(msgHex, 'hex') : Buffer.alloc(0);
  let sig = Buffer.from(curve.sign(priv, msg));
  // libsignal's curve25519-js returns the signature with the sign bit already
  // moved into the last byte; some builds return it as a plain Uint8Array.
  if (sig.length !== 64) sig = Buffer.from(sig.buffer, sig.byteOffset, 64);
  if (sig.length !== 64) throw new Error('unexpected signature length ' + sig.length);
  lines.push(`XEDDSA ${privHex} ${msg.length ? h(msg) : '-'} ${h(sig)} 1`);
}

// A malformed signature that must be rejected (separate kind: no signing
// comparison is possible for it).
{
  const bad = Buffer.alloc(64, 0x11);
  lines.push(`XEDDSA_BAD ${cases[0][0]} ${cases[0][1]} ${h(bad)}`);
}

fs.writeFileSync('/home/user/wa2me/qt/tests/sig_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
