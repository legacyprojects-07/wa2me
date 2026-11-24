// Phase 2/3 of the Signal interop test.
//
// Rebuilds "Bob" (the responder) from qt/tests/signal_vectors.txt, then:
//   * decrypts the PreKeyWhisperMessage produced by the C++ port
//   * checks the plaintext is what the C++ side encrypted
//   * encrypts a reply and writes it for the C++ side to decrypt
//
// Usage: node qt/tests/verify_signal.mjs <vectors> <cppMessageIn> <replyOut>
import fs from 'fs';
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
// libsignal is resolved from BAILEYS_DIR's node_modules (it is a dependency of
// @whiskeysockets/baileys), so the test does not need its own install.
const baileysDir = process.env.BAILEYS_DIR;
if (!baileysDir) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const libsignal = createRequire(baileysDir + '/package.json')('libsignal');

const [, , vectorsPath, cppIn, replyOut] = process.argv;
const lines = fs.readFileSync(vectorsPath, 'utf8').split('\n').filter(Boolean);

let bobIdentity = null, bobRegId = 0;
const signed = {}, preKeys = {};
for (const line of lines) {
  const parts = line.split(' ');
  const hx = s => Buffer.from(s, 'hex');
  if (parts[0] === 'BOBKEY') {
    bobIdentity = { privKey: hx(parts[1]), pubKey: hx(parts[2]) };
    bobRegId = Number(parts[3]);
  } else if (parts[0] === 'BOBSIGNED') {
    signed[Number(parts[1])] = { privKey: hx(parts[2]), pubKey: hx(parts[3]) };
  } else if (parts[0] === 'BOBPREKEY') {
    preKeys[Number(parts[1])] = { privKey: hx(parts[2]), pubKey: hx(parts[3]) };
  }
}

class MemStore {
  constructor(identity, regId) {
    this.identity = identity; this.regId = regId;
    this.sessions = new Map(); this.preKeys = new Map(); this.signedPreKeys = new Map();
  }
  async getOurIdentity() { return this.identity; }
  async getOurRegistrationId() { return this.regId; }
  async isTrustedIdentity() { return true; }
  async loadSession(id) { const s = this.sessions.get(id); return s ? libsignal.SessionRecord.deserialize(s) : undefined; }
  async storeSession(id, rec) { this.sessions.set(id, rec.serialize()); }
  async loadPreKey(id) { return this.preKeys.get(String(id)); }
  async storePreKey(id, kp) { this.preKeys.set(String(id), kp); }
  async removePreKey(id) { this.preKeys.delete(String(id)); }
  async loadSignedPreKey(id) { return this.signedPreKeys.get(String(id)) || this.signedPreKeys.values().next().value; }
  async storeSignedPreKey(id, kp) { this.signedPreKeys.set(String(id), kp); }
}

const store = new MemStore(bobIdentity, bobRegId);
for (const [id, kp] of Object.entries(preKeys)) await store.storePreKey(Number(id), kp);
for (const [id, kp] of Object.entries(signed)) await store.storeSignedPreKey(Number(id), kp);

const cppLine = fs.readFileSync(cppIn, 'utf8').trim().split(' ');
if (cppLine[0] !== 'CPPMSG') throw new Error('unexpected input: ' + cppLine[0]);
const type = Number(cppLine[1]);
const body = Buffer.from(cppLine[2], 'hex');

const aliceAddr = new libsignal.ProtocolAddress('+10000000001', 1);
const cipher = new libsignal.SessionCipher(store, aliceAddr);

const plaintext = type === 3
  ? await cipher.decryptPreKeyWhisperMessage(body, 'binary')
  : await cipher.decryptWhisperMessage(body, 'binary');

console.log(`libsignal decrypted the C++ ciphertext: "${plaintext.toString('utf8')}"`);
if (plaintext.toString('utf8') !== 'hi bob') {
  console.error('FAIL: unexpected plaintext from the C++ side');
  process.exit(1);
}

const reply = await cipher.encrypt(Buffer.from('reply from bob', 'utf8'));
fs.writeFileSync(replyOut,
  `JSREPLY ${reply.type} ${Buffer.from(reply.body).toString('hex')} ` +
  `${Buffer.from('reply from bob', 'utf8').toString('hex')}\n`);
console.log('C++ ciphertext accepted by libsignal; reply written for the C++ side');
