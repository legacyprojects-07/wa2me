// Phase 2 of qt/tests/test_message.cpp.
//
//   * rebuilds Bob from message_vectors.txt
//   * decodes the <message> stanza the C++ client produced (Baileys' own
//     BinaryNode decoder), decrypts it with libsignal and checks the text
//   * encrypts two replies for the C++ identity and writes them back as
//     encoded stanzas (a pre-key message, then a plain whisper message, plus a
//     deviceSentMessage-wrapped copy)
//
// Usage: node qt/tests/verify_message.mjs <vectors> <cppDir>
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const require2 = createRequire(BAILEYS_DIR + '/package.json');
const libsignal = require2('libsignal');
const { proto } = await import(BAILEYS_DIR + '/WAProto/index.js');
const { decodeBinaryNode } = await import(BAILEYS_DIR + '/lib/WABinary/decode.js');
const { encodeBinaryNode } = await import(BAILEYS_DIR + '/lib/WABinary/encode.js');

const [, , vectorsPath, cppDir] = process.argv;
const hx = s => Buffer.from(s, 'hex');
const h = b => Buffer.from(b).toString('hex');

// ── Baileys' unpadRandomMax16 ─────────────────────────────────────────────
const unpad = buf => buf.subarray(0, buf.length - buf[buf.length - 1]);
const pad = msg => {
  const padLength = (Math.floor(Math.random() * 16) + 1);
  return Buffer.concat([msg, Buffer.alloc(padLength, padLength)]);
};

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

// ── Bob's secrets ─────────────────────────────────────────────────────────
let bobIdentity = null, bobRegId = 0;
const bobSigned = {}, bobPreKeys = {};
for (const line of fs.readFileSync(vectorsPath, 'utf8').split('\n').filter(Boolean)) {
  const p = line.split(' ');
  if (p[0] === 'BOBKEY') {
    bobIdentity = { privKey: hx(p[1]), pubKey: hx(p[2]) };
    bobRegId = Number(p[3]);
  } else if (p[0] === 'BOBSIGNED') {
    bobSigned[Number(p[1])] = { privKey: hx(p[2]), pubKey: hx(p[3]) };
  } else if (p[0] === 'BOBPREKEY') {
    bobPreKeys[Number(p[1])] = { privKey: hx(p[2]), pubKey: hx(p[3]) };
  }
}
if (!bobIdentity) throw new Error('no BOBKEY vector');

// ── What the C++ client wrote ─────────────────────────────────────────────
const cpp = {};
for (const line of fs.readFileSync(cppDir + '/cpp_message.txt', 'utf8').split('\n').filter(Boolean)) {
  const [kind, ...rest] = line.split(' ');
  cpp[kind] = rest;
}
const aliceAddress = new libsignal.ProtocolAddress('919999999999', 1);

let failures = 0;
const check = (ok, what, extra = '') => {
  if (ok) { console.log('ok   ' + what); return; }
  failures++;
  console.error('FAIL ' + what + (extra ? ' — ' + extra : ''));
};

// ── 1. Decode + decrypt the stanza produced by the C++ client ─────────────
const bobStore = new MemStore(bobIdentity, bobRegId);
for (const [id, kp] of Object.entries(bobSigned)) await bobStore.storeSignedPreKey(Number(id), kp);
for (const [id, kp] of Object.entries(bobPreKeys)) await bobStore.storePreKey(Number(id), kp);

const stanza = await decodeBinaryNode(hx(cpp.CPPSTANZA[0]));
check(stanza.tag === 'message', 'stanza tag');
check(stanza.attrs.type === 'text', 'stanza type');
check(stanza.attrs.to === '5511@s.whatsapp.net', 'stanza to', stanza.attrs.to);
const participants = stanza.content.find(n => n.tag === 'participants');
check(!!participants, 'participants node');
const to = participants.content.find(n => n.tag === 'to');
check(to.attrs.jid === '5511@s.whatsapp.net', 'participant jid', to.attrs.jid);
const enc = to.content.find(n => n.tag === 'enc');
check(enc.attrs.v === '2', 'enc v');
check(enc.attrs.type === 'pkmsg', 'enc type pkmsg', enc.attrs.type);

const bobCipher = new libsignal.SessionCipher(bobStore, aliceAddress);
const decrypted = await bobCipher.decryptPreKeyWhisperMessage(enc.content, 'binary');
const message = proto.Message.decode(unpad(Buffer.from(decrypted)));
check(message.conversation === 'hello from qt', 'libsignal decrypted the C++ text',
      JSON.stringify(message.conversation));

// The wrapped copy (deviceSentMessage) must decrypt too.
const decryptAny = async (cipher, enc) => unpad(Buffer.from(
  enc.attrs.type === 'pkmsg'
    ? await cipher.decryptPreKeyWhisperMessage(enc.content, 'binary')
    : await cipher.decryptWhisperMessage(enc.content, 'binary')));

const wrappedStanza = await decodeBinaryNode(hx(cpp.CPPWRAPPED[0]));
const wrappedEnc = wrappedStanza.content.find(n => n.tag === 'enc');
const wrappedPlain = await decryptAny(bobCipher, wrappedEnc);
const wrapped = proto.Message.decode(wrappedPlain);
check(!!wrapped.deviceSentMessage, 'deviceSentMessage present');
check(wrapped.deviceSentMessage.destinationJid === '5511@s.whatsapp.net',
      'deviceSent destination', wrapped.deviceSentMessage?.destinationJid);
check(wrapped.deviceSentMessage.message.conversation === 'hello from qt',
      'deviceSent inner text');

// ── 2. Encrypt replies for the C++ identity ───────────────────────────────
const cppIde = {
  identityKey: hx(cpp.CPPIDENTITY[1]),
  registrationId: Number(cpp.CPPIDENTITY[2]),
  signedPreKey: { keyId: Number(cpp.CPPSIGNED[0]), publicKey: hx(cpp.CPPSIGNED[1]), signature: hx(cpp.CPPSIGNED[2]) },
  preKey: { keyId: Number(cpp.CPPPREKEY[0]), publicKey: hx(cpp.CPPPREKEY[1]) },
};

const replyStore = new MemStore(bobIdentity, bobRegId);
await new libsignal.SessionBuilder(replyStore, aliceAddress).initOutgoing(cppIde);
const replyCipher = new libsignal.SessionCipher(replyStore, aliceAddress);

const makeStanza = (id, type, body, extraContent = []) => ({
  tag: 'message',
  attrs: { id, from: '5511@s.whatsapp.net', t: '1700001234', type: 'text' },
  content: [{ tag: 'enc', attrs: { v: '2', type }, content: body }, ...extraContent],
});

const encodeReply = async (cipher, text) => {
  const ct = await cipher.encrypt(pad(proto.Message.encode(
    proto.Message.fromObject({ conversation: text })).finish()));
  return { type: ct.type === 3 ? 'pkmsg' : 'msg', body: Buffer.from(ct.body) };
};

// libsignal keeps sending pre-key messages until the peer answers, so both of
// these are pkmsg — the C++ client must handle either type at any time.
const first = await encodeReply(replyCipher, 'hello from node');
const node1 = encodeBinaryNode(makeStanza('NODE1', first.type, first.body));

const second = await encodeReply(replyCipher, 'second from node');
const node2 = encodeBinaryNode(makeStanza('NODE2', second.type, second.body));

// A deviceSentMessage-wrapped copy, as another of our own devices would send
// it. It rides the session opened above (a second pre-key session would reuse
// the already-consumed one-time pre-key, which real bundles never do).
const wrappedInner = proto.Message.fromObject({
  deviceSentMessage: {
    destinationJid: '5511@s.whatsapp.net',
    message: { conversation: 'hello from qt' },
  },
});
const wrappedCt = await replyCipher.encrypt(pad(proto.Message.encode(wrappedInner).finish()));
const node3 = encodeBinaryNode(makeStanza('NODE3', wrappedCt.type === 3 ? 'pkmsg' : 'msg',
  Buffer.from(wrappedCt.body)));

fs.writeFileSync(cppDir + '/node_message.txt', `NODESTANZA ${h(node1)}\n`);
fs.writeFileSync(cppDir + '/node_message2.txt', `NODESTANZA2 ${h(node2)}\n`);
fs.writeFileSync(cppDir + '/node_wrapped.txt', `NODEWRAPPED ${h(node3)}\n`);

console.log('');
console.log(failures === 0 ? 'MESSAGE INTEROP OK' : `MESSAGE INTEROP FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
