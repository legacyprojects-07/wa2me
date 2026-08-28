# wa2me

A self-hosted WhatsApp HTTP API built on [Baileys](https://github.com/WhiskeySockets/Baileys). Exposes WhatsApp as a REST API that any HTTP client can consume — designed for low-resource clients like Qt 4.7.3 on Symbian, but works with anything that speaks HTTP.

**Stack:** Node.js · Express · Baileys · PostgreSQL (Render)

---

## Table of Contents

- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [API Reference](#api-reference)
  - [Connection & Health](#connection--health)
  - [Authentication (QR)](#authentication-qr)
  - [Chats](#chats)
  - [Messages](#messages)
  - [Media](#media)
  - [Send](#send)
  - [Presence](#presence)
  - [Contacts](#contacts)
  - [Profile Pictures](#profile-pictures)
- [Data Models](#data-models)
- [JID Format](#jid-format)
- [Persistence & Storage](#persistence--storage)
- [Environment Variables](#environment-variables)
- [Project Structure](#project-structure)
- [Internals](#internals)
- [Limitations](#limitations)

---

## Quick Start

### Local Development

```bash
git clone https://github.com/legacyprojects-07/wa2me.git
cd wa2me
npm install

# Set DATABASE_URL to your PostgreSQL connection string
cp .env.example .env
# Edit .env

node index.js
```

Open `http://localhost:3000/qr-image` in a browser. Scan the QR code with WhatsApp → Settings → Linked Devices → Link a Device.

### Deploy on Render (free)

1. Push to GitHub
2. [render.com](https://render.com) → **New** → **Blueprint** → connect repo
3. Render auto-creates a PostgreSQL database + web service
4. Open `https://your-app.onrender.com/qr-image` → scan QR

---

## Architecture

```
┌──────────────┐     HTTP/JSON     ┌──────────────┐    WebSocket    ┌──────────┐
│  Any Client  │ ◄─────────────── │    wa2me     │ ◄─────────────► │ WhatsApp │
│  (Qt, curl,  │   REST responses  │   (Node.js)  │   Baileys lib   │ Servers  │
│   browser)   │                   └──────┬───────┘                 └──────────┘
└──────────────┘                          │
                                   ┌──────┴───────┐
                                   │  PostgreSQL  │
                                   │   (Render)   │
                                   └──────────────┘
```

- **Baileys** maintains a persistent WebSocket to WhatsApp as a linked device
- **Express** exposes HTTP endpoints for clients
- **PostgreSQL** stores chats, contacts, messages (survives redeploys)
- **Disk cache** stores downloaded media (ephemeral on Render free tier)
- **Memory** stores presence (online/typing) state

---

## API Reference

All JSON responses use ASCII-safe encoding — non-ASCII characters are escaped as `\uXXXX` to prevent UTF-8 corruption on legacy clients like Symbian/J2ME.

### Connection & Health

#### `GET /health`

Returns server status, connected user info, and database statistics. No authentication required.

```bash
curl https://your-app.onrender.com/health
```

**Response:**
```json
{
  "status": "connected",
  "user": {
    "id": "919467087568:42@s.whatsapp.net",
    "name": "Ashwin"
  },
  "historySyncComplete": true,
  "stats": {
    "chats": 33,
    "contacts": 156,
    "messages": 14523
  }
}
```

| `status` value | Meaning |
|---|---|
| `starting` | Server is booting, Baileys not yet initialized |
| `qr` | Waiting for QR code scan |
| `connected` | WhatsApp linked and ready |
| `disconnected` | Connection lost, auto-retrying |
| `loggedOut` | Removed from Linked Devices, need re-scan |

---

### Authentication (QR)

#### `GET /qr`

QR code as a base64 data URL in JSON. Useful for embedding in apps.

```bash
curl https://your-app.onrender.com/qr
```

**Response (waiting):**
```json
{
  "status": "qr",
  "qr": "data:image/png;base64,iVBORw0KGgo..."
}
```

**Response (already connected):**
```json
{
  "status": "connected",
  "qr": null
}
```

---

#### `GET /qr-image`

QR code as a PNG image. Open in a browser to scan with your phone.

| Param  | Type | Default | Description |
|--------|------|---------|-------------|
| `size` | int  | 256     | Image width in pixels (80–500) |

```bash
# Open in browser
https://your-app.onrender.com/qr-image

# Custom size
https://your-app.onrender.com/qr-image?size=400
```

**Response:** `image/png`

---

### Chats

#### `GET /chats`

List all chats sorted by most recent message.

| Param    | Type | Default | Description |
|----------|------|---------|-------------|
| `all`    | `1`  | —       | Include unsaved contacts |
| `limit`  | int  | 200     | Max results (max 500) |
| `offset` | int  | 0       | Pagination offset |

```bash
curl https://your-app.onrender.com/chats
curl https://your-app.onrender.com/chats?all=1&limit=50
```

**Response:**
```json
[
  {
    "id": "918814915654@s.whatsapp.net",
    "name": "Arjun",
    "unreadCount": 0,
    "lastMessageTs": 1787911215
  },
  {
    "id": "919812704002@s.whatsapp.net",
    "name": "Sunil Ji",
    "unreadCount": 2,
    "lastMessageTs": 1787911214
  },
  {
    "id": "120363428676422887@g.us",
    "name": "Study Group",
    "unreadCount": 0,
    "lastMessageTs": 1787911214
  }
]
```

By default only saved contacts and groups are returned. Unsaved numbers (like `+91 98125 06061`) only appear with `?all=1`.

Groups are identified by `@g.us` suffix in the `id`. Individual contacts use `@s.whatsapp.net`.

---

#### `GET /chats/:jid`

Single chat detail with contact info and current presence.

```bash
curl https://your-app.onrender.com/chats/918814915654@s.whatsapp.net
```

**Response:**
```json
{
  "jid": "918814915654@s.whatsapp.net",
  "name": "Arjun",
  "isGroup": false,
  "unreadCount": 0,
  "lastMessageTs": 1787911215,
  "lastMessage": "Ok bye",
  "messageCount": 247,
  "contact": {
    "name": "Arjun",
    "notify": "Arjun",
    "phoneNumber": "918814915654",
    "isSaved": true
  },
  "presence": {
    "status": "unavailable",
    "lastSeen": 1787911200000,
    "updatedAt": 1787911200000
  }
}
```

---

#### `POST /chats/:jid/read`

Mark a chat as read. Resets `unreadCount` to 0 and sends read receipts to WhatsApp so the sender sees blue ticks.

```bash
curl -X POST https://your-app.onrender.com/chats/919812704002@s.whatsapp.net/read
```

**Response:**
```json
{ "ok": true }
```

---

### Messages

#### `GET /messages/:jid`

Get messages for a chat. Returns messages in chronological order (oldest first). Automatically marks the chat as read.

| Param    | Type | Default | Description |
|----------|------|---------|-------------|
| `limit`  | int  | 100     | Max messages (max 500) |
| `before` | int  | —       | Unix timestamp — get messages older than this |

```bash
# Latest 100 messages
curl https://your-app.onrender.com/messages/120363428676422887@g.us

# Pagination — get messages before a timestamp
curl "https://your-app.onrender.com/messages/120363428676422887@g.us?before=1787138000&limit=50"
```

**Response:**
```json
[
  {
    "id": "AC2081DE9CBEB93B43B8DF2181FE484F",
    "fromMe": true,
    "timestamp": 1787136776,
    "text": "Hello",
    "mediaType": null,
    "hasMedia": false,
    "mediaUrl": null
  },
  {
    "id": "ACA8C25D52B6F1B5D993F49F727E51AE",
    "fromMe": false,
    "timestamp": 1787137951,
    "text": "Kisko",
    "mediaType": null,
    "hasMedia": false,
    "mediaUrl": null
  },
  {
    "id": "AC330545E45C56AB71B0019CAFB946DB",
    "fromMe": true,
    "timestamp": 1787150731,
    "text": "[Photo]",
    "mediaType": "image",
    "hasMedia": true,
    "mediaUrl": "/messages/120363428676422887%40g.us/AC330545E45C56AB71B0019CAFB946DB/media"
  },
  {
    "id": "3EB0230A8B924B390065BE",
    "fromMe": false,
    "timestamp": 1787138084,
    "text": "https://us05web.zoom.us/j/82939004731?pwd=cJBVDQdKbbWR8bK1v7QWrQC6mFnasy.1",
    "mediaType": null,
    "hasMedia": false,
    "mediaUrl": null
  }
]
```

**Message fields:**

| Field | Type | Description |
|---|---|---|
| `id` | string | WhatsApp message ID (unique within a chat) |
| `fromMe` | boolean | `true` if you sent this message |
| `timestamp` | number | Unix timestamp (seconds since epoch) |
| `text` | string | Message text, caption, or placeholder |
| `mediaType` | string \| null | `image`, `video`, `audio`, `document`, `sticker`, or `null` |
| `hasMedia` | boolean | `true` if downloadable media is available |
| `mediaUrl` | string \| null | API path to download the media file |

**Text placeholders:**
- `[Photo]` — image with no caption
- `[Video]` — video with no caption
- `[Audio]` — voice note or audio file
- `[Document]` — file attachment
- `[Message]` — unsupported message type (reactions, contacts, locations, etc.)
- `[Contact]` — shared contact card
- `[Location]` — shared location

---

### Media

#### `GET /messages/:jid/:msgId/media`

Download the media file for a message. Returns the raw file, not JSON.

| Param  | Type | Description |
|--------|------|-------------|
| `thumb` | `1` | Return thumbnail instead of full resolution |

```bash
# Download full image
curl -o photo.jpg "https://your-app.onrender.com/messages/120363428676422887%40g.us/AC330545E45C56AB71B0019CAFB946DB/media"

# Download thumbnail (small, fast)
curl -o thumb.jpg "https://your-app.onrender.com/messages/120363428676422887%40g.us/AC330545E45C56AB71B0019CAFB946DB/media?thumb=1"
```

**Response headers:**
```
Content-Type: image/jpeg          (or video/mp4, audio/ogg, etc.)
Cache-Control: public, max-age=604800
```

**Fallback chain** (if full media unavailable):
1. Serve from local disk cache
2. Download from WhatsApp on-demand (works for recent messages)
3. Serve thumbnail (`jpegThumbnail` embedded in message)
4. Return `404` with `{"error": "Media not available or expired"}`

---

### Send

#### `POST /send`

Send a text message or base64-encoded media.

**Content-Type:** `application/json`

**Send text:**
```bash
curl -X POST https://your-app.onrender.com/send \
  -H "Content-Type: application/json" \
  -d '{"jid":"918814915654@s.whatsapp.net","text":"Hello from wa2me!"}'
```

**Send image (base64):**
```bash
curl -X POST https://your-app.onrender.com/send \
  -H "Content-Type: application/json" \
  -d '{
    "jid": "120363428676422887@g.us",
    "mediaType": "image",
    "mediaBase64": "'$(base64 -w0 photo.jpg)'",
    "caption": "Check this out!"
  }'
```

**Request body:**

| Field | Type | Required | Description |
|---|---|---|---|
| `jid` | string | **yes** | Recipient (individual or group JID) |
| `text` | string | * | Text message |
| `mediaType` | string | * | `image`, `video`, `audio`, `document` |
| `mediaBase64` | string | * | Base64-encoded file data |
| `caption` | string | no | Caption for images/videos |
| `mimetype` | string | no | MIME type (e.g. `image/jpeg`) |
| `fileName` | string | no | Filename for documents |

\* Either `text` OR `mediaBase64` + `mediaType` is required.

**Response:**
```json
{
  "ok": true,
  "id": "AC582F05EC066F84E4A111364E9DCC56",
  "timestamp": 1787911220
}
```

**Error responses:**
```json
{"error": "jid is required"}
{"error": "Either text or mediaBase64+mediaType required"}
{"error": "Not connected (disconnected)"}
```

---

#### `POST /send-media`

Send media via multipart form upload. Better for large files.

**Content-Type:** `multipart/form-data`

| Field | Type | Required | Description |
|---|---|---|---|
| `jid` | string | **yes** | Recipient JID |
| `file` | file | **yes** | File to send (max 16 MB) |
| `caption` | string | no | Caption for images/videos |
| `mediaType` | string | no | Override auto-detection |

```bash
curl -X POST https://your-app.onrender.com/send-media \
  -F "jid=120363428676422887@g.us" \
  -F "file=@screenshot.png" \
  -F "caption=Here's the screenshot"
```

Media type is auto-detected from the file's MIME type:
- `image/*` → image
- `video/*` → video
- `audio/*` → audio
- everything else → document

**Response:**
```json
{
  "ok": true,
  "id": "AC6242016E1860006D752F7925CABDE4",
  "timestamp": 1787911225
}
```

---

### Presence

#### `GET /presence/:jid`

Get online/typing status for a contact or group.

```bash
# Individual contact
curl https://your-app.onrender.com/presence/918814915654@s.whatsapp.net

# Group (returns all participants)
curl https://your-app.onrender.com/presence/120363428676422887@g.us
```

**Individual response:**
```json
{
  "status": "composing",
  "lastSeen": null,
  "updatedAt": 1787911220000
}
```

**Group response** (keyed by participant JID):
```json
{
  "918814915654@s.whatsapp.net": {
    "status": "available",
    "lastSeen": null,
    "updatedAt": 1787911220000
  },
  "919812704002@s.whatsapp.net": {
    "status": "composing",
    "lastSeen": null,
    "updatedAt": 1787911221000
  }
}
```

**Status values:**

| Status | Meaning |
|---|---|
| `available` | Online — WhatsApp is open and in foreground |
| `composing` | Currently typing a message |
| `recording` | Recording a voice note |
| `paused` | Was typing/recording, stopped |
| `unavailable` | Offline |

Presence is in-memory only. Entries expire after 5 minutes of no update. **Poll this endpoint every 3–5 seconds** while displaying a chat to the user.

---

#### `GET /presence`

Get ALL currently active presences across all chats. Only returns non-expired, non-unavailable entries.

```bash
curl https://your-app.onrender.com/presence
```

**Response:**
```json
{
  "918814915654@s.whatsapp.net": {
    "status": "available",
    "lastSeen": null,
    "updatedAt": 1787911220000
  },
  "120363428676422887@g.us:919812704002@s.whatsapp.net": {
    "status": "composing",
    "lastSeen": null,
    "updatedAt": 1787911221000
  }
}
```

Group participant keys use the format `<groupJid>:<participantJid>`.

---

### Contacts

#### `GET /contacts`

List all known contacts. Includes saved contacts (from your phone's address book) and unsaved contacts (discovered from group messages via `pushName`).

```bash
curl https://your-app.onrender.com/contacts
```

**Response:**
```json
[
  {
    "jid": "918814915654@s.whatsapp.net",
    "name": "Arjun",
    "notify": "Arjun",
    "phoneNumber": "918814915654",
    "isSaved": true
  },
  {
    "jid": "919812704002@s.whatsapp.net",
    "name": "Sunil Ji",
    "notify": "Sunil",
    "phoneNumber": "919812704002",
    "isSaved": true
  },
  {
    "jid": "919812506061@s.whatsapp.net",
    "name": "+91 98125 06061",
    "notify": "",
    "phoneNumber": "",
    "isSaved": false
  }
]
```

| Field | Type | Description |
|---|---|---|
| `jid` | string | WhatsApp JID |
| `name` | string | Display name (contact name, or phone number if unsaved) |
| `notify` | string | Name from WhatsApp pushName (set by the user themselves) |
| `phoneNumber` | string | Phone number (empty for some contacts) |
| `isSaved` | boolean | `true` if saved in your phone's address book |

---

### Profile Pictures

#### `GET /profile-pic/:jid`

Get a user's or group's profile picture as JPEG.

```bash
curl -o pfp.jpg https://your-app.onrender.com/profile-pic/918814915654@s.whatsapp.net
```

**Response:** `image/jpeg` with `Cache-Control: public, max-age=3600`

Returns `404` with `{"error": "No profile picture"}` if the user has no profile picture set.

---

## Data Models

### Chat Object (from `GET /chats`)

```json
{
  "id": "919812704002@s.whatsapp.net",
  "name": "Sunil Ji",
  "unreadCount": 2,
  "lastMessageTs": 1787911214
}
```

| Field | Type | Description |
|---|---|---|
| `id` | string | JID — unique identifier for the chat |
| `name` | string | Display name (contact name or group subject) |
| `unreadCount` | number | Unread message count (0 if read) |
| `lastMessageTs` | number | Unix timestamp of the most recent message |

### Message Object (from `GET /messages/:jid`)

```json
{
  "id": "AC8FD2E100DBB22D4E304D114F481398",
  "fromMe": true,
  "timestamp": 1787136220,
  "text": "[Message]",
  "mediaType": null,
  "hasMedia": false,
  "mediaUrl": null
}
```

| Field | Type | Description |
|---|---|---|
| `id` | string | WhatsApp message ID (unique per chat, not globally) |
| `fromMe` | boolean | Whether you sent this |
| `timestamp` | number | Unix timestamp in seconds |
| `text` | string | Text content, caption, or placeholder |
| `mediaType` | string \| null | `image` · `video` · `audio` · `document` · `sticker` · `null` |
| `hasMedia` | boolean | Whether downloadable media exists |
| `mediaUrl` | string \| null | Relative API path to download media |

### Presence Object (from `GET /presence/:jid`)

```json
{
  "status": "composing",
  "lastSeen": null,
  "updatedAt": 1787911220000
}
```

| Field | Type | Description |
|---|---|---|
| `status` | string | `available` · `composing` · `recording` · `paused` · `unavailable` |
| `lastSeen` | number \| null | Unix timestamp (ms) when user went offline. `null` if online or unknown. |
| `updatedAt` | number \| null | Unix timestamp (ms) of last presence update |

---

## JID Format

WhatsApp uses JIDs (Jabber IDs) to identify entities.

| Type | Format | Example |
|---|---|---|
| Individual contact | `<phone>@s.whatsapp.net` | `918814915654@s.whatsapp.net` |
| Group | `<id>@g.us` | `120363428676422887@g.us` |

**Shorthand:** You can pass just the phone number (e.g. `918814915654`) and the server appends `@s.whatsapp.net` automatically. Groups always need the full `@g.us` JID.

**How to tell them apart:**
- Ends with `@s.whatsapp.net` → individual chat
- Ends with `@g.us` → group chat

---

## Persistence & Storage

| Data | Storage | Survives restart | Survives redeploy (git push) |
|---|---|---|---|
| Chats | PostgreSQL | ✅ | ✅ |
| Contacts | PostgreSQL | ✅ | ✅ |
| Messages | PostgreSQL | ✅ | ✅ |
| Unread counts | PostgreSQL | ✅ | ✅ |
| Message delivery status | PostgreSQL | ✅ | ✅ |
| WhatsApp auth session | Local disk (`auth_state/`) | ✅ | ❌ — re-scan QR |
| Media cache | Local disk (`data/media/`) | ✅ | ❌ — re-downloads on demand |
| Online/typing presence | In-memory | ❌ | ❌ |

After a redeploy, scan the QR code at `/qr-image`. All chat history is still in PostgreSQL. Baileys history sync fills in any messages that arrived while the server was down.

---

## Environment Variables

| Variable | Required | Default | Description |
|---|---|---|---|
| `DATABASE_URL` | **yes** | — | PostgreSQL connection string. Render sets this automatically when you link a database. |
| `PORT` | no | `3000` | HTTP server port. Render sets this automatically. |
| `AUTH_DIR` | no | `./auth_state` | Directory for Baileys WhatsApp credentials. |
| `MEDIA_DIR` | no | `./data/media` | Directory for downloaded media cache. |
| `API_KEY` | no | — | If set, all requests (except `/health` and `/qr-image`) require `X-API-Key` header or `?api_key=` query param. |
| `LOG_LEVEL` | no | `silent` | Pino log level: `silent` `fatal` `error` `warn` `info` `debug` `trace` |

---

## Project Structure

```
├── index.js         # Express server, all HTTP routes, boot sequence
├── wa.js            # Baileys socket, WhatsApp events, send functions, presence
├── db.js            # PostgreSQL schema, migrations, all queries
├── media.js         # Media download to disk, cache lookup, thumbnails
├── helpers.js       # Timestamp parsing, message unwrapping, JID utils, safeJSON
├── render.yaml      # Render deployment blueprint (web + postgres)
├── package.json
├── .env.example
├── .gitignore
└── README.md
```

| Module | Responsibility |
|---|---|
| `index.js` | HTTP layer only. Defines routes, parses requests, sends responses. Delegates all logic to `wa.js` and `db.js`. |
| `wa.js` | WhatsApp layer. Manages Baileys connection, handles all events (messages, contacts, chats, presence, read receipts). Exposes `sendText()`, `sendMedia()`, `markChatRead()`. |
| `db.js` | Database layer. Schema migrations, CRUD for chats/contacts/messages. All functions are async (PostgreSQL). |
| `media.js` | Media layer. Downloads from WhatsApp to disk, serves from cache, generates thumbnails, handles expired media fallback. |
| `helpers.js` | Pure utilities. No state, no side effects. |

---

## Internals

### Connection Lifecycle

1. `index.js` boots → initializes PostgreSQL → starts Baileys
2. Baileys loads auth from `AUTH_DIR` (or generates new credentials)
3. Opens WebSocket to WhatsApp servers
4. If no auth exists → QR code generated → served at `/qr-image`
5. User scans QR → WhatsApp links the device
6. `messaging-history.set` fires → all chats, contacts, messages synced into PostgreSQL
7. Real-time events stream in: new messages, presence changes, read receipts, contact updates

### Message Lifecycle (incoming)

```
WhatsApp server
  → Baileys WebSocket
  → messages.upsert event
  → recordMessage()
    → parseMessageContent() extracts text/media
    → INSERT into messages table
    → UPDATE chats (last_message, last_message_ts, unread_count)
  → if hasMedia: downloadMediaBackground() → save to disk
```

### Message Lifecycle (outgoing)

```
Client → POST /send {jid, text}
  → wa.sendText()
    → sock.presenceUpdate('composing')  [typing indicator]
    → sock.sendMessage(jid, {text})
    → sock.presenceUpdate('paused')
    → recordMessage() → INSERT into messages
  → WhatsApp delivers → messages.update event → status = 'delivered'
  → Recipient reads → messages.update event → status = 'read'
```

### Presence Lifecycle

```
WhatsApp server
  → Baileys WebSocket
  → presence.update event
  → updatePresence() → stored in-memory Map
  → Client polls GET /presence/:jid every 3-5s
  → Returns current status (available/composing/recording/paused/unavailable)
```

### Media Lifecycle

```
Incoming media message:
  1. Message recorded in PostgreSQL (hasMedia=true, mediaPath=null)
  2. Raw Baileys message cached in memory (24h TTL, max 5000 entries)
  3. Background: downloadMediaMessage() → write to disk → UPDATE mediaPath in PostgreSQL

Client requests GET /messages/:jid/:msgId/media:
  1. Check PostgreSQL for mediaPath → serve from disk
  2. If not cached: try on-demand download from WhatsApp (raw msg cache)
  3. If download fails: serve jpegThumbnail if available
  4. If nothing available: 404
```

---

## Limitations

| Limitation | Details |
|---|---|
| One WhatsApp account per server | Baileys connects one account. Run separate instances for multiple numbers. |
| No push/webhook to clients | Clients must poll for new messages. No WebSocket or webhook push from server to client. |
| Media ephemeral on Render free | Media cache lost on redeploys. Re-downloads on demand from WhatsApp (fails for media >30 days old). |
| Presence is in-memory | Online/typing status lost on restart. Not persisted to database. |
| WhatsApp rate limits | Sending too many messages too fast may get the account temporarily banned. |
| History sync is one-time | Full history synced on first connect. After that, only real-time messages arrive. |
| No message search | No full-text search endpoint. Messages are stored but not indexed for search. |
| Edited messages | Content updates are stored, but no edit history — only the latest version is kept. |
