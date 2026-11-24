// Phase 2 of qt/tests/test_sender_key.cpp.
//
//   * Baileys' GroupCipher decrypts the group message the C++ client produced
//     (proving our sender-key derivation + XEdDSA signature are right)
//   * a fresh sender key on Baileys' side is distributed back, with one group
//     message, so the C++ side can prove its *receive* path on new state
//
// Usage: BAILEYS_DIR=... node qt/tests/verify_group.mjs <vectors> <dir>
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const [, , vectorsPath, dir] = process.argv;
const h = b => Buffer.from(b).toString('hex');

// See gen_sender_key.mjs: load the package entry point first (module cycle).
const { proto } = await import(BAILEYS_DIR + '/WAProto/index.js');
await import(BAILEYS_DIR + '/lib/index.js');
const { GroupCipher, GroupSessionBuilder, SenderKeyDistributionMessage, SenderKeyName, SenderKeyRecord } =
  await import(BAILEYS_DIR + '/lib/Signal/Group/index.js');
const { ProtocolAddress } = createRequire(BAILEYS_DIR + '/package.json')('libsignal');

const group = '120363041234567890@g.us';
const cpp = {};
for (const line of fs.readFileSync(dir + '/cpp_group.txt', 'utf8').split('\n').filter(Boolean)) {
  const [kind, value] = line.split(' ');
  cpp[kind] = value;
}

let failures = 0;
const check = (ok, what, extra = '') => {
  if (ok) { console.log('ok   ' + what); return; }
  failures++;
  console.error('FAIL ' + what + (extra ? ' — ' + extra : ''));
};

class SenderKeyStore {
  constructor() { this.map = new Map(); }
  async loadSenderKey(name) { return this.map.get(name.toString()); }
  async storeSenderKey(name, record) { this.map.set(name.toString(), record); }
}

// ── 1. Decrypt the C++ message ────────────────────────────────────────────
const meName = new SenderKeyName(group, new ProtocolAddress('919999999999', 1));
const store = new SenderKeyStore();
await store.storeSenderKey(meName, new SenderKeyRecord());
await new GroupSessionBuilder(store).process(
  meName, new SenderKeyDistributionMessage(null, null, null, null, Buffer.from(cpp.CPPSKDM, 'hex')));

const plaintext = await new GroupCipher(store, meName).decrypt(Buffer.from(cpp.CPPSKMSG, 'hex'));
check(h(plaintext) === cpp.CPPPAYLOAD, 'Baileys decrypted the C++ group message', h(plaintext));

// ── 2. Send something back from a fresh sender key ────────────────────────
const bobName = new SenderKeyName(group, new ProtocolAddress('5511', 0));
const bobStore = new SenderKeyStore();
await bobStore.storeSenderKey(bobName, new SenderKeyRecord());
const dist = await new GroupSessionBuilder(bobStore).create(bobName);
const body = Buffer.concat([Buffer.from('hello qt group', 'utf8'), Buffer.from([0x03])]);
const skmsg = await new GroupCipher(bobStore, bobName).encrypt(body);

// The message id/iteration the C++ side will see.
const decoded = proto.SenderKeyMessage.decode(skmsg.slice(1, skmsg.length - 64));
check(decoded.iteration === 0, 'node message starts at iteration 0', String(decoded.iteration));

fs.writeFileSync(dir + '/node_group.txt',
  `NODEDECRYPTED ${h(plaintext)}\n` +
  `NODESKDM ${h(dist.serialize())}\n` +
  `NODEGROUPMSG ${h(skmsg)} ${h(body)}\n`);

console.log('');
console.log(failures === 0 ? 'GROUP INTEROP (node side) OK' : `GROUP INTEROP FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
