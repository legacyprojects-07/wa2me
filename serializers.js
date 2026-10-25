'use strict';

const db = require('./db');
const { phoneFromJid } = require('./helpers');

function num(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function mediaUrl(chatJid, msgId) {
  return `/messages/${encodeURIComponent(chatJid)}/${encodeURIComponent(msgId)}/media`;
}

/**
 * Chat list payload. Kept backward compatible with v5 (jid/name/unreadCount/
 * lastMessageTs/lastMessage) and adds archive/pin/mute flags for v6 clients.
 */
function chat(c) {
  const jid = c.jid;
  return {
    jid,
    id: jid, // legacy v5 clients used `id`; keep both forever
    name: c.name || c.contact_name || c.contact_notify || (jid ? jid.replace(/@.*/, '') : ''),
    isGroup: Boolean(c.is_group),
    unreadCount: num(c.unread_count, 0),
    lastMessageTs: num(c.last_message_ts, 0),
    lastMessage: c.last_message || '',
    archived: Boolean(c.is_archived),
    pinned: Boolean(c.is_pinned),
    muted: Boolean(c.is_muted),
    muteUntil: num(c.mute_until, 0),
  };
}

function contact(c) {
  if (!c) return null;
  return {
    jid: c.jid,
    name: c.name || c.notify || c.phone_number || String(c.jid || '').replace(/@.*/, ''),
    notify: c.notify || '',
    phoneNumber: c.phone_number || phoneFromJid(c.jid),
    isSaved: Boolean(c.is_saved),
  };
}

function baseMessage(m) {
  const deleted = Boolean(m.deleted);
  const chatJid = m.chat_jid;
  const id = m.id;
  const hasMedia = !deleted && Boolean(m.has_media);

  return {
    id,
    chatJid,
    fromMe: Boolean(m.from_me),
    senderJid: m.sender_jid || '',
    senderName: m.sender_name || '',
    timestamp: num(m.timestamp, 0),
    text: deleted ? '' : (m.text || ''),
    mediaType: deleted ? null : (m.media_type || null),
    hasMedia,
    mediaUrl: hasMedia ? mediaUrl(chatJid, id) : null,
    mediaMime: deleted ? null : (m.media_mime || null),
    fileName: deleted ? null : (m.media_name || null),
    mediaSeconds: deleted ? null : (m.media_seconds ? num(m.media_seconds) : null),
    isPtt: deleted ? false : Boolean(m.is_ptt),
    status: m.status || 'received',
    deleted,
    deletedAt: m.deleted_at ? num(m.deleted_at) : null,
    edited: Boolean(m.edited),
    quotedId: m.quoted_id || null,
    quoted: null,
    reactions: [],
    reactionSummary: [],
  };
}

function summary(reactions) {
  const counts = new Map();
  for (const r of reactions) {
    if (!r.emoji) continue;
    counts.set(r.emoji, (counts.get(r.emoji) || 0) + 1);
  }
  return Array.from(counts, ([emoji, count]) => ({ emoji, count }));
}

/**
 * Attach reactions and quoted-message context with two batched queries per
 * chat instead of N+1 queries per message. Use this for every list endpoint.
 */
async function decorateMessages(rows) {
  if (!rows || rows.length === 0) return [];

  const byChat = new Map();
  for (const row of rows) {
    const jid = row.chat_jid || '';
    if (!byChat.has(jid)) byChat.set(jid, []);
    byChat.get(jid).push(row);
  }

  const reactionsByChat = new Map();
  const quotesByChat = new Map();

  await Promise.all(Array.from(byChat.entries()).map(async ([jid, chatRows]) => {
    const ids = chatRows.map((r) => r.id);
    const quotedIds = Array.from(new Set(chatRows.map((r) => r.quoted_id).filter(Boolean)));
    const [reactions, quotes] = await Promise.all([
      db.getReactions(jid, ids),
      db.getQuotedMessages(jid, quotedIds),
    ]);
    reactionsByChat.set(jid, reactions);
    quotesByChat.set(jid, new Map(quotes.map((q) => [q.id, q])));
  }));

  return rows.map((row) => {
    const out = baseMessage(row);
    const chatJid = row.chat_jid || '';
    const reactions = (reactionsByChat.get(chatJid) || [])
      .filter((r) => r.message_id === row.id)
      .map((r) => ({ senderJid: r.sender_jid || '', emoji: r.emoji, fromMe: Boolean(r.from_me) }));
    out.reactions = out.deleted ? [] : reactions;
    out.reactionSummary = out.deleted ? [] : summary(reactions);

    const quoted = quotesByChat.get(chatJid)?.get(row.quoted_id);
    if (quoted) {
      out.quoted = {
        id: quoted.id,
        fromMe: Boolean(quoted.from_me),
        senderName: quoted.sender_name || '',
        mediaType: quoted.media_type || null,
        deleted: Boolean(quoted.deleted),
        text: quoted.deleted ? '' : (quoted.text || ''),
      };
    }
    return out;
  });
}

function group(g) {
  if (!g) return null;
  return {
    jid: g.id,
    subject: g.subject || '',
    description: g.desc || '',
    owner: g.ownerJid || g.owner || null,
    createdAt: g.creation || null,
    isAnnounce: Boolean(g.announce),
    isLocked: Boolean(g.restrict),
    isCommunity: Boolean(g.isCommunity),
    size: g.size || (g.participants ? g.participants.length : 0),
    participants: (g.participants || []).map((p) => ({
      jid: p.id,
      phoneNumber: p.phoneNumber || phoneFromJid(p.id),
      name: p.name || p.notify || '',
      admin: p.admin || null,
      isAdmin: Boolean(p.isAdmin || p.admin === 'admin' || p.admin === 'superadmin'),
      isSuperAdmin: Boolean(p.isSuperAdmin || p.admin === 'superadmin'),
    })),
  };
}

module.exports = { chat, contact, baseMessage, decorateMessages, group };
