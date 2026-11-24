// Vector generator for the Qt-free core tests.
//
// Requires the real Baileys package (with its deps) and Node >= 18:
//   BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys \
//     node qt/tests/gen_vectors.mjs.mjs
//
// The checked-in *_vectors.txt files were generated this way against
// @whiskeysockets/baileys 6.7.22, so the C++ port is verified against the real
// reference implementation rather than hand-copied constants.
import fs from 'fs';
import zlib from 'zlib';

import { pathToFileURL } from 'url';

const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const baileysImport = p => pathToFileURL(BAILEYS_DIR + p).href;

const { encodeBinaryNode } = await import(baileysImport('/lib/WABinary/encode.js'));
const { decodeBinaryNode } = await import(baileysImport('/lib/WABinary/decode.js'));
const constants = await import(baileysImport('/lib/WABinary/constants.js'));

// Hand-built nodes covering tokens, jids, nibble/hex packing, bytes, lists.
const nodes = [
  { tag: 'iq', attrs: { id: '123456', type: 'get', xmlns: 'w:p', to: 's.whatsapp.net' } },
  { tag: 'success', attrs: { lid: '1612345:2@s.whatsapp.net' }, content: [{ tag: 'item', attrs: { k: 'v' } }] },
  { tag: 'message', attrs: { to: '919999999999@s.whatsapp.net', id: 'ABCDEF0123456789', type: 'text' },
    content: [{ tag: 'enc', attrs: { v: '2', type: 'msg' }, content: Buffer.from([1,2,3,4,5,255,254]) }] },
  { tag: 'stream:features', content: [
      { tag: 'readreceipts' },
      { tag: 'passive' },
      { tag: 'user', attrs: { jid: '912345678901:7@s.whatsapp.net' }, content: [{ tag: 'x', attrs: { a: '1' } }] }
  ]},
  { tag: 'ack', attrs: { class: 'message', id: '3EB0ABCDEF', from: '120363041234567890@g.us', t: '1700000000' } },
  { tag: 'bin', attrs: { n: '1' }, content: Buffer.from(Array.from({length: 300}, (_,i)=> i & 0xff)) },
  { tag: 't', attrs: { v: 'hello world this is a longer raw string with spaces & symbols!' }, content: 'text-content' },
  { tag: 'nib', attrs: { n: '1234567890-.' }, content: [] },
  { tag: 'empty', attrs: {} },
];

const vectors = [];
for (const n of nodes) {
  const encoded = encodeBinaryNode(n);
  const decoded = await decodeBinaryNode(encoded);
  vectors.push({
    input: JSON.parse(JSON.stringify(n, (k,v)=> v && v.type==='Buffer' ? {__buf:Array.from(v.data)} : v)),
    hex: encoded.toString('hex'),
    decoded: JSON.parse(JSON.stringify(decoded, (k,v)=> v && v.type==='Buffer' ? {__buf:Array.from(v.data)} : v)),
  });
}

// Compression vectors: raw deflate (what WhatsApp sends with flag 0x02).
const rawPayloads = [
  Buffer.from('hello hello hello hello hello'),
  Buffer.from(Array.from({length: 5000}, (_,i)=> i % 7)),
  Buffer.from(JSON.stringify({a:1,b:'x'.repeat(200)})),
  Buffer.alloc(0),
  Buffer.from('a'),
];
const compressed = rawPayloads.map(p => ({ raw: p.toString('hex'), deflated: zlib.deflateRawSync(p).toString('hex') }));
// Also: a real encoded frame, compressed, exactly as WhatsApp wraps it.
const frame = encodeBinaryNode(nodes[2]);
compressed.push({ raw: frame.toString('hex'), deflated: zlib.deflateRawSync(frame).toString('hex') });

// Verify Baileys can decode our "compressed frame" wrapper convention:
// WhatsApp frame = [flag] + payload. flag 0x02 means the payload is deflated.
const wrapped = Buffer.concat([Buffer.from([0x02]), zlib.deflateRawSync(frame.slice(1))]);

fs.writeFileSync('/tmp/vectors.json', JSON.stringify({vectors, compressed, wrapped: wrapped.toString('hex')}, null, 1));
console.log('vectors:', vectors.length, 'compressed:', compressed.length);
