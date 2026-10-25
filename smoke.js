'use strict';



process.env.NODE_ENV = 'test';
process.env.API_KEY = '';

const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');

const ROOT = path.resolve(__dirname, '..');
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'openknot-test-'));
process.env.UPLOAD_TMP_DIR = path.join(TMP, 'uploads');
process.env.MEDIA_DIR = path.join(TMP, 'media');
process.env.AUTH_DIR = path.join(TMP, 'auth');

const CHAT_JID = '919999999999@s.whatsapp.net';
const GROUP_JID = '120363000000000000@g.us';

const chat = {
  jid: CHAT_JID,
  name: 'Test User',
  is_group: false,
  unread_count: 2,
  last_message_ts: '1700000100',
  last_message: 'hello',
  is_archived: false,
  is_pinned: true,
  is_muted: false,
  mute_until: 0,
  contact_name: 'Test User',
  contact_notify: 'Test',
};

const msg = {
  id: 'MSG1',
  chat_jid: CHAT_JID,
  sender_jid: CHAT_JID,
  sender_name: 'Test User',
  from_me: false,
  timestamp: '1700000100',
  text: 'hello',
  media_type: null,
  has_media: false,
  media_path: null,
  media_mime: null,
  media_name: null,
  media_seconds: null,
  is_ptt: false,
  status: 'received',
  deleted: false,
  deleted_at: null,
  edited: false,
  quoted_id: null,
  updated_at: new Date('2026-10-25T00:00:00Z'),
  raw_json: null,
};

const mediaPath = path.join(TMP, 'test.jpg');
fs.writeFileSync(mediaPath, Buffer.from([0xff, 0xd8, 0xff, 0xd9]));
const mediaMsg = {
  ...msg,
  id: 'MEDIA1',
  text: 'pic',
  media_type: 'image',
  has_media: true,
  media_path: mediaPath,
  media_mime: 'image/jpeg',
  media_name: 'photo.jpg',
};

const calls = [];

const db = {
  getStats: async () => ({ chats: 1, contacts: 1, messages: 2 }),
  listChats: async () => [chat],
  listGroups: async () => [],
  getChat: async () => chat,
  getContact: async () => ({
    jid: CHAT_JID, name: 'Test User', notify: 'Test',
    phone_number: '919999999999', is_saved: true,
  }),
  getMessageCount: async () => 2,
  getLatestMessages: async () => [msg, mediaMsg],
  getMessages: async () => [msg, mediaMsg],
  getMessage: async (jid, id) => (
    id === 'MEDIA1' ? mediaMsg : (id === 'MSG1' ? msg : null)
  ),
  getReactions: async () => [
    { message_id: 'MSG1', sender_jid: CHAT_JID, emoji: '👍', from_me: false },
  ],
  getQuotedMessages: async () => [],
  getMessagesUpdatedSince: async () => [msg, mediaMsg],
  getChatsUpdatedSince: async () => [chat],
  getPool: () => ({ query: async () => ({ rows: [{ c: 1 }], rowCount: 1 }) }),
  setUnreadCount: async (...args) => calls.push(['setUnreadCount', ...args]),
};

function okMessage(id, timestamp, mediaType = null) {
  return { key: { id }, messageTimestamp: timestamp, mediaType };
}

const wa = {
  getStatus: () => ({
    status: 'connected',
    user: { id: '911111111111:1@s.whatsapp.net', name: 'Me' },
    historySyncComplete: true,
  }),
  getPresence: () => ({ status: 'available' }),
  getAllPresences: () => ({}),
  getQR: () => null,
  getMe: () => ({ id: '911111111111:1@s.whatsapp.net', name: 'Me', phoneNumber: '911111111111' }),
  getAbout: async () => ({ status: 'Busy', setAt: 1700000000 }),
  markChatRead: async (jid) => calls.push(['read', jid]),
  markChatUnread: async (jid, n) => ({ warnings: [], jid, n }),
  sendPresenceUpdate: async () => {},
  subscribePresence: async () => ({ subscribed: true }),
  sendText: async (jid, text, opts) => {
    calls.push(['sendText', jid, text, opts]);
    return okMessage('NEWTEXT', 1700000200);
  },
  sendMedia: async (jid, opts) => {
    calls.push(['sendMedia', jid, opts]);
    return okMessage('NEWMEDIA', 1700000201, opts.mediaType);
  },
  sendContact: async () => okMessage('NEWCONTACT', 1700000202),
  sendLocation: async () => okMessage('NEWLOC', 1700000203),
  deleteForEveryone: async (jid, id) => {
    calls.push(['deleteEveryone', jid, id]);
    return { alreadyDeleted: false };
  },
  deleteForMe: async (jid, id) => {
    calls.push(['deleteMe', jid, id]);
    return { deleted: true };
  },
  editMessage: async (jid, id, text) => ({ text }),
  sendReaction: async (jid, id, emoji) => ({ emoji, removed: !emoji }),
  forwardMessage: async (from, id, to) => {
    calls.push(['forward', from, id, to]);
    return okMessage('FWD', 1700000204);
  },
  setChatArchived: async (j, x) => ({ archived: x, warnings: [] }),
  setChatPinned: async (j, x) => ({ pinned: x, warnings: [] }),
  setChatMuted: async (j, m, d) => ({ muted: m, muteUntil: 0, warnings: [] }),
  clearChat: async () => ({ cleared: true, warnings: [] }),
  deleteChat: async () => ({ deleted: true, warnings: [] }),
  getGroupMetadata: async () => ({ id: GROUP_JID, subject: 'G', participants: [] }),
  createGroup: async () => ({ id: GROUP_JID, subject: 'G', participants: [] }),
  updateGroupParticipants: async () => [],
  leaveGroup: async () => ({ left: true }),
  getGroupInviteCode: async () => ({ code: 'abc', link: 'https://chat.whatsapp.com/abc' }),
  revokeGroupInvite: async () => ({ code: 'def', link: 'https://chat.whatsapp.com/def' }),
  joinGroup: async () => ({ jid: GROUP_JID, code: 'abc' }),
  updateGroupSubject: async (j, s) => ({ subject: s }),
  updateGroupDescription: async (j, d) => ({ description: d }),
  setGroupSetting: async (j, s) => ({ setting: s }),
  updateProfileStatus: async (s) => ({ status: s }),
  updateProfileName: async (n) => ({ name: n }),
  setOwnPresence: async (s) => ({ status: s }),
  checkOnWhatsApp: async (p) => ({ input: p, jid: CHAT_JID, exists: true, lid: null }),
  getBlocklist: async () => [CHAT_JID],
  setBlocked: async (j, b) => ({ jid: j, blocked: b }),
  getProfilePicUrl: async () => null,
  downloadMediaOnDemand: async () => null,
};

const dbPath = path.join(ROOT, 'db.js');
const waPath = path.join(ROOT, 'wa.js');
require.cache[dbPath] = { id: dbPath, filename: dbPath, loaded: true, exports: db };
require.cache[waPath] = { id: waPath, filename: waPath, loaded: true, exports: wa };

const { app } = require(path.join(ROOT, 'index.js'));

async function main() {
  const server = app.listen(0);
  await new Promise((resolve) => server.once('listening', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;

  const request = async (res) => ({ status: res.status, body: await res.json() });

  try {
    let r = await request(await fetch(`${base}/health`));
    assert.equal(r.status, 200);
    assert.equal(r.body.status, 'connected');
    assert.equal(r.body.version, '6.0.0');

    r = await request(await fetch(`${base}/chats`));
    assert.equal(r.body[0].id, CHAT_JID, 'legacy id field');
    assert.equal(r.body[0].jid, CHAT_JID);
    assert.equal(r.body[0].pinned, true);

    r = await request(await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}`));
    assert.equal(r.body.length, 2);
    assert.equal(r.body[0].reactions[0].emoji, '👍');
    assert.equal(r.body[1].mediaUrl.includes('MEDIA1'), true);

    // multipart file + form fields
    const form = new FormData();
    form.append('jid', CHAT_JID);
    form.append('caption', 'uploaded');
    form.append('mediaType', 'image');
    form.append('file', new Blob([Buffer.from('fake-image')], { type: 'image/jpeg' }), 'photo.jpg');
    r = await request(await fetch(`${base}/send`, { method: 'POST', body: form }));
    assert.equal(r.status, 200);
    assert.equal(r.body.id, 'NEWMEDIA');
    const sendCall = calls.find((c) => c[0] === 'sendMedia');
    assert.equal(sendCall[2].buffer.toString(), 'fake-image');
    assert.equal(sendCall[2].mediaType, 'image');

    // JSON text
    r = await request(await fetch(`${base}/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ jid: CHAT_JID, text: 'yo' }),
    }));
    assert.equal(r.body.id, 'NEWTEXT');

    // delete for everyone / me
    r = await request(await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MSG1/delete`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ scope: 'everyone' }),
    }));
    assert.equal(r.body.scope, 'everyone');
    r = await request(await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MSG1/delete`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ scope: 'me' }),
    }));
    assert.equal(r.body.scope, 'me');

    // forward, react, edit, contact, location
    r = await request(await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MSG1/forward`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ to: '918888888888' }),
    }));
    assert.equal(r.body.id, 'FWD');

    r = await request(await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MSG1/react`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ emoji: '❤️' }),
    }));
    assert.equal(r.body.emoji, '❤️');

    r = await request(await fetch(`${base}/send-contact`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ jid: CHAT_JID, name: 'Friend', phone: '918888888888' }),
    }));
    assert.equal(r.body.id, 'NEWCONTACT');

    r = await request(await fetch(`${base}/sync?since=0`));
    assert.equal(r.status, 200);
    assert.equal(r.body.messages.length, 2);
    assert.equal(r.body.nextSince > 0, true);

    const mediaRes = await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MEDIA1/media`);
    assert.equal(mediaRes.status, 200);
    assert.equal(mediaRes.headers.get('content-type'), 'image/jpeg');
    assert.equal(mediaRes.headers.get('content-disposition').includes('photo.jpg'), true);

    const thumbRes = await fetch(`${base}/messages/${encodeURIComponent(CHAT_JID)}/MEDIA1/media?thumb=1`);
    assert.equal(thumbRes.status, 200);

    r = await request(await fetch(`${base}/check/919999999999`));
    assert.equal(r.body.exists, true);

    r = await request(await fetch(`${base}/groups/${encodeURIComponent(GROUP_JID)}`));
    assert.equal(r.body.jid, GROUP_JID);

    r = await request(await fetch(`${base}/does-not-exist`));
    assert.equal(r.status, 404);

    console.log('OpenKnot smoke test: all checks passed');
  } finally {
    server.close();
    fs.rmSync(TMP, { recursive: true, force: true });
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
