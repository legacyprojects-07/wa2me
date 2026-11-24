// Generates a libsignal session exchange for qt/tests/test_signal.cpp.
// Requires BAILEYS_DIR (the @whiskeysockets/baileys package directory).
// Generates a full libsignal session exchange so the C++ port can be checked
// in both directions against the reference implementation.
import crypto from 'crypto';
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
// Resolve libsignal exactly as Baileys itself does.
const libsignal = createRequire(BAILEYS_DIR + '/package.json')('libsignal');
const KeyHelper = libsignal.keyhelper;

// Minimal in-memory storage adapter, matching the interface libsignal expects
// (the same shape Baileys' signalStorage provides).
class MemStore {
  constructor(identity, regId) {
    this.identity = identity;
    this.regId = regId;
    this.sessions = new Map();
    this.preKeys = new Map();
    this.signedPreKeys = new Map();
  }
  async getOurIdentity() { return this.identity; }
  async getOurRegistrationId() { return this.regId; }
  async isTrustedIdentity() { return true; }
  async loadSession(id) { const s = this.sessions.get(id); return s ? libsignal.SessionRecord.deserialize(s) : undefined; }
  async storeSession(id, record) { this.sessions.set(id, record.serialize()); }
  async loadPreKey(id) { return this.preKeys.get(String(id)); }
  async storePreKey(id, kp) { this.preKeys.set(String(id), kp); }
  async removePreKey(id) { this.preKeys.delete(String(id)); }
  async loadSignedPreKey(id) { return this.signedPreKeys.get(String(id)) || this.signedPreKeys.values().next().value; }
  async storeSignedPreKey(id, kp) { this.signedPreKeys.set(String(id), kp); }
}

const aliceAddr = new libsignal.ProtocolAddress('+10000000001', 1);
const bobAddr = new libsignal.ProtocolAddress('+10000000002', 1);

// Bob's keys (the responder)
const bobIdentity = KeyHelper.generateIdentityKeyPair();
const bobRegId = KeyHelper.generateRegistrationId(false);
const bobSignedPreKey = KeyHelper.generateSignedPreKey(bobIdentity, 5);
const bobPreKey = KeyHelper.generatePreKey(31337);

const store = new MemStore(bobIdentity, bobRegId);
await store.storePreKey(bobPreKey.keyId, bobPreKey.keyPair);
await store.storeSignedPreKey(bobSignedPreKey.keyId, bobSignedPreKey.keyPair);

// Alice's keys (the initiator)
const aliceIdentity = KeyHelper.generateIdentityKeyPair();
const aliceRegId = KeyHelper.generateRegistrationId(false);
const aliceStore = new MemStore(aliceIdentity, aliceRegId);
const aliceSignedPreKey = KeyHelper.generateSignedPreKey(aliceIdentity, 7);
await aliceStore.storeSignedPreKey(aliceSignedPreKey.keyId, aliceSignedPreKey.keyPair);

// Bob's public bundle, as WhatsApp would hand it to Alice
const bobBundle = {
  identityKey: bobIdentity.pubKey,
  registrationId: bobRegId,
  signedPreKey: {
    keyId: bobSignedPreKey.keyId,
    publicKey: bobSignedPreKey.keyPair.pubKey,
    signature: bobSignedPreKey.signature,
  },
  preKey: { keyId: bobPreKey.keyId, publicKey: bobPreKey.keyPair.pubKey },
};

const lines = [];
const h = b => Buffer.from(b).toString('hex');

// ── Bob's secrets for the C++ side ────────────────────────────────────────
lines.push(`BOBKEY ${h(bobIdentity.privKey)} ${h(bobIdentity.pubKey)} ${bobRegId}`);
lines.push(`BOBSIGNED ${bobSignedPreKey.keyId} ${h(bobSignedPreKey.keyPair.privKey)} ${h(bobSignedPreKey.keyPair.pubKey)} ${h(bobSignedPreKey.signature)}`);
lines.push(`BOBPREKEY ${bobPreKey.keyId} ${h(bobPreKey.keyPair.privKey)} ${h(bobPreKey.keyPair.pubKey)}`);
lines.push(`BOBBUNDLE ${h(bobBundle.identityKey)} ${bobRegId} ${bobSignedPreKey.keyId} ${h(bobSignedPreKey.keyPair.pubKey)} ${h(bobSignedPreKey.signature)} ${bobPreKey.keyId} ${h(bobPreKey.keyPair.pubKey)}`);

// ── Alice encrypts to Bob with the JS library ─────────────────────────────
const builder = new libsignal.SessionBuilder(aliceStore, bobAddr);
await builder.initOutgoing(bobBundle);
const aliceCipher = new libsignal.SessionCipher(aliceStore, bobAddr);

const plaintexts = [
  'hello from libsignal',
  'second message on the same chain',
  'third message after the ratchet has advanced',
];
const jsMessages = [];
for (const text of plaintexts) {
  const ct = await aliceCipher.encrypt(Buffer.from(text, 'utf8'));
  jsMessages.push({ type: ct.type, body: Buffer.from(ct.body).toString('hex'), text });
}
for (const m of jsMessages) lines.push(`JSMSG ${m.type} ${m.body} ${Buffer.from(m.text).toString('hex')}`);

// Bob decrypts them (proves the messages are valid) and replies, producing a
// JS->JS reply the C++ side must be able to decrypt with the session state
// established by the pre-key message above.
const bobCipher = new libsignal.SessionCipher(store, aliceAddr);
for (const m of jsMessages) {
  const pt = await bobCipher.decryptPreKeyWhisperMessage(Buffer.from(m.body, 'hex'), 'binary');
  if (pt.toString('utf8') !== m.text) throw new Error('JS self-check failed');
}
const reply = await bobCipher.encrypt(Buffer.from('reply from bob', 'utf8'));
lines.push(`JSREPLY ${reply.type} ${Buffer.from(reply.body).toString('hex')} ${Buffer.from('reply from bob').toString('hex')}`);

// Alice's identity so the C++ side can address her
lines.push(`ALICEKEY ${h(aliceIdentity.privKey)} ${h(aliceIdentity.pubKey)} ${aliceRegId}`);

fs.writeFileSync('/home/user/wa2me/qt/tests/signal_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
