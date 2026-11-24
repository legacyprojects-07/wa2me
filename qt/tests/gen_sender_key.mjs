// Generates group (sender-key) vectors for qt/tests/test_sender_key.cpp using
// Baileys' *own* group implementation (lib/Signal/Group/*), which is the code
// WhatsApp Web itself uses.
//
// Usage: BAILEYS_DIR=... node qt/tests/gen_sender_key.mjs
import fs from 'fs';
import { createRequire } from 'module';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const { proto } = await import(BAILEYS_DIR + '/WAProto/index.js');
// Baileys' lib entry must be evaluated first: the group classes pull in
// Utils/generics.js, which is part of a module cycle that only resolves when
// the package is loaded through its documented entry point.
await import(BAILEYS_DIR + '/lib/index.js');
const { GroupCipher, GroupSessionBuilder, SenderKeyDistributionMessage, SenderKeyName, SenderKeyRecord } =
  await import(BAILEYS_DIR + '/lib/Signal/Group/index.js');
const { ProtocolAddress } = createRequire(BAILEYS_DIR + '/package.json')('libsignal');

const h = b => Buffer.from(b).toString('hex');

class SenderKeyStore {
  constructor() { this.map = new Map(); }
  async loadSenderKey(name) { return this.map.get(name.toString()); }
  async storeSenderKey(name, record) { this.map.set(name.toString(), record); }
}

const group = '120363041234567890@g.us';
const bob = new SenderKeyName(group, new ProtocolAddress('5511', 0));
const store = new SenderKeyStore();
await store.storeSenderKey(bob, new SenderKeyRecord());

const builder = new GroupSessionBuilder(store);
const dist = await builder.create(bob);
const cipher = new GroupCipher(store, bob);

const lines = [];
const parsed = new SenderKeyDistributionMessage(null, null, null, null, dist.serialize());
lines.push(`SKDM ${parsed.getId()} ${parsed.getIteration()} ${h(parsed.getChainKey())} ` +
           `${h(parsed.getSignatureKey())} ${h(dist.serialize())}`);

// Bob sends three group messages. Baileys' GroupCipher skips every other
// iteration after the first (0, 2, 4…), which is exactly the case a receiver
// must resynchronise from.
const texts = ['first group message', 'second group message', 'third after the gap'];
for (const text of texts) {
  const body = Buffer.concat([Buffer.from(text, 'utf8'), Buffer.from([0x01])]);
  const skmsg = await cipher.encrypt(body);
  const decoded = proto.SenderKeyMessage.decode(skmsg.slice(1, skmsg.length - 64));
  lines.push(`GROUPMSG ${decoded.iteration} ${h(skmsg)} ${h(body)}`);
}

// A tampered copy of the last message: the signature must be rejected.
{
  const body = Buffer.concat([Buffer.from('tampered', 'utf8'), Buffer.from([0x01])]);
  const skmsg = Buffer.from(await cipher.encrypt(body));
  skmsg[skmsg.length - 1] ^= 0xff;
  lines.push(`BADMSG ${h(skmsg)}`);
}

// A second sender key (as a client does after a re-key), for the phase-3
// "fresh distribution" check.
{
  const bob2 = new SenderKeyName(group, new ProtocolAddress('5511', 0));
  const store2 = new SenderKeyStore();
  await store2.storeSenderKey(bob2, new SenderKeyRecord());
  const builder2 = new GroupSessionBuilder(store2);
  const dist2 = await builder2.create(bob2);
  const cipher2 = new GroupCipher(store2, bob2);
  const body = Buffer.concat([Buffer.from('after re-key', 'utf8'), Buffer.from([0x01])]);
  const skmsg = await cipher2.encrypt(body);
  lines.push(`REKEYSKDM ${h(dist2.serialize())}`);
  lines.push(`REKEYMSG ${h(skmsg)} ${h(body)}`);
}

fs.writeFileSync('/home/user/wa2me/qt/tests/sender_key_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
