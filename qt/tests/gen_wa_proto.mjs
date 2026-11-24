// Generates WAProto message/history vectors for qt/tests/test_wa_proto.cpp.
// Requires BAILEYS_DIR (the @whiskeysockets/baileys package directory).
import fs from 'fs';
import { pathToFileURL } from 'url';
const BAILEYS_DIR = process.env.BAILEYS_DIR;
if (!BAILEYS_DIR) {
  console.error('set BAILEYS_DIR to the @whiskeysockets/baileys package directory');
  process.exit(1);
}
const { proto } = await import(pathToFileURL(BAILEYS_DIR + '/WAProto/index.js').href);
const h = b => Buffer.from(b).toString('hex');
const lines = [];

// 1. Message with conversation
const conv = proto.Message.encode(proto.Message.fromObject({ conversation: 'hello world' })).finish();
lines.push(`MSG_CONV ${h(conv)} ${Buffer.from('hello world').toString('hex')}`);

// 2. Extended text with a quoted message (ContextInfo)
// ContextInfo.quotedMessage is an *embedded Message* (not bytes), so build it
// as a plain object and let protobufjs recurse. Passing an already-encoded
// Buffer here silently yields an empty submessage.
const quotedObj = { conversation: 'the original' };
const quoted = proto.Message.encode(proto.Message.fromObject(quotedObj)).finish();
const ext = proto.Message.encode(proto.Message.fromObject({
  extendedTextMessage: {
    text: 'replying to this',
    contextInfo: {
      stanzaId: 'ABCD1234',
      participant: '919999999999@s.whatsapp.net',
      quotedMessage: quotedObj,
    },
  },
})).finish();
lines.push(`MSG_EXT ${h(ext)} ${Buffer.from('replying to this').toString('hex')} ${h(quoted)} ABCD1234 919999999999@s.whatsapp.net`);

// 3. Image message metadata
const img = proto.Message.encode(proto.Message.fromObject({
  imageMessage: {
    url: 'https://mmg.whatsapp.net/x',
    mimetype: 'image/jpeg',
    caption: 'look at this',
    fileSha256: Buffer.alloc(32, 7),
    fileLength: 12345,
    mediaKey: Buffer.alloc(32, 9),
    directPath: '/mms/image/abc',
    jpegThumbnail: Buffer.from([1, 2, 3, 4]),
  },
})).finish();
lines.push(`MSG_IMG ${h(img)} ${Buffer.from('look at this').toString('hex')} image/jpeg 12345 ${h(Buffer.alloc(32,9))} /mms/image/abc`);

// 4. HistorySync with conversations, messages and pushnames
const mkMsg = (id, text, ts, fromMe, participant) => proto.WebMessageInfo.fromObject({
  key: { remoteJid: '919999999999@s.whatsapp.net', fromMe, id, participant },
  message: { conversation: text },
  messageTimestamp: ts,
});
const history = proto.HistorySync.encode(proto.HistorySync.fromObject({
  syncType: 0,
  conversations: [
    {
      id: '120363041234567890@g.us',
      name: 'Family',
      unreadCount: 3,
      conversationTimestamp: 1700000000,
      messages: [
        { message: mkMsg('MSG1', 'first in group', 1699999000, false, '918888888888@s.whatsapp.net') },
        { message: mkMsg('MSG2', 'my reply', 1699999500, true, undefined) },
      ],
    },
    {
      id: '919999999999@s.whatsapp.net',
      unreadCount: 0,
      conversationTimestamp: 1700000500,
      messages: [{ message: mkMsg('MSG3', 'direct message', 1700000500, false, undefined) }],
    },
  ],
  pushnames: [
    { id: '918888888888@s.whatsapp.net', pushname: 'Ravi' },
    { id: '919999999999@s.whatsapp.net', pushname: 'Me' },
  ],
  progress: 42,
})).finish();
lines.push(`HISTORY ${h(history)} 42 2 MSG1 ${Buffer.from('first in group').toString('hex')} Family 120363041234567890@g.us MSG3 ${Buffer.from('direct message').toString('hex')} 918888888888@s.whatsapp.net ${Buffer.from('Ravi').toString('hex')}`);

// 5. HistorySyncNotification (protocol message inside an incoming message)
const note = proto.Message.HistorySyncNotification.encode(proto.Message.HistorySyncNotification.fromObject({
  fileSha256: Buffer.alloc(32, 1),
  fileLength: 999,
  mediaKey: Buffer.alloc(32, 2),
  directPath: '/mms/md-app-state/xyz',
  syncType: 3,
  chunkOrder: 2,
  progress: 55,
})).finish();
lines.push(`NOTIFY ${h(note)} 999 ${h(Buffer.alloc(32,2))} /mms/md-app-state/xyz 3 2 55`);

fs.writeFileSync('/home/user/wa2me/qt/tests/wa_proto_vectors.txt', lines.join('\n') + '\n');
console.log('wrote', lines.length, 'lines');
