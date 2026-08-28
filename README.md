# wa2me

A self-hosted WhatsApp HTTP API built on [Baileys](https://github.com/WhiskeySockets/Baileys). Exposes WhatsApp as a REST API that any client can consume — designed for low-resource clients like Qt 4.7.3 on Symbian, but works with anything that speaks HTTP.

**Stack:** Node.js + Express + Baileys + PostgreSQL (Render)

---

## Table of Contents

- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [API Reference](#api-reference)
  - [Connection](#connection)
  - [Chats](#chats)
  - [Messages](#messages)
  - [Media](#media)
  - [Send](#send)
  - [Presence](#presence)
  - [Contacts & Profile](#contacts--profile)
- [Data Models](#data-models)
- [JID Format](#jid-format)
- [Persistence](#persistence)
- [Environment Variables](#environment-variables)
- [Project Structure](#project-structure)
- [How It Works](#how-it-works)
- [Limitations](#limitations)
- [License](#license)

---

## Quick Start

### Prerequisites

- [Node.js](https://nodejs.org/) >= 18
- PostgreSQL database (or [Render free tier](https://render.com))
- WhatsApp account with an active phone number

### Local Development

```bash
git clone https://github.com/legacyprojects-07/wa2me.git
cd wa2me
npm install

# Set up your database
cp .env.example .env
# Edit .env and set DATABASE_URL to your PostgreSQL connection string

node index.js
```

Open `http://localhost:3000/qr-image` and scan the QR code with WhatsApp (Settings → Linked Devices → Link a Device).

### Deploy on Render

1. Push to GitHub
2. On [render.com](https://render.com) → **New** → **Blueprint** → connect your repo
3. Render creates a free PostgreSQL database + web service automatically
4. Open `https://your-app.onrender.com/qr-image` → scan QR

---

## Architecture

```
┌──────────────┐     HTTP/REST      ┌──────────────┐     WebSocket     ┌──────────┐
│  Qt Client   │ ◄──────────────── │   wa2me      │ ◄──────────────► │ WhatsApp │
│  (Symbian)   │    JSON responses  │   (Node.js)  │   Baileys lib    │  Servers │
└──────────────┘                    └──────┬───────┘                   └──────────┘
                                           │
                                    ┌──────┴───────┐
                                    │  PostgreSQL  │
                                    │  (Render)    │
                                    └──────────────┘
```

- **Baileys** maintains a WebSocket connection to WhatsApp's servers
- **Express** exposes HTTP endpoints for clients
- **PostgreSQL** stores chats, contacts, and messages persistently
- **Media cache** stores downloaded images/videos/audio on local disk
- **Presence tracker** keeps online/typing status in memory

---

## API Reference

All responses use ASCII-safe JSON (`\uXXXX` for non-ASCII characters) to prevent UTF-8 corruption on legacy clients like Symbian.

### Connection

#### `GET /health`

Server health check. No auth required.

**Response:**
```json
{
  "status": "connected",
  "user": {
    "id": "919876543210:42@s.whatsapp.net",
    "name": "Your Name"
  },
  "historySyncComplete": true,
  "stats": {
    "chats": 156,
    "contacts": 312,
    "messages": 14523
  }
}
```

**Status values:** `starting` | `qr` | `connected` | `disconnected` | `loggedOut`

---

#### `GET /qr`

QR code as a base64 data URL (JSON).

**Response (waiting for scan):**
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

QR code as a PNG image. Open this URL in a browser to scan.

| Param  | Type  | Default | Description          |
|--------|-------|---------|----------------------|
| `size` | int   | 256     | Image width (80-500) |

**Response:** `image/png`

---

### Chats

#### `GET /chats`

List all chats, sorted by most recent message.

| Param  | Type  | Default | Description                          |
|--------|-------|---------|--------------------------------------|
| `all`  | `1`   | —       | Include unsaved contacts             |
| `limit`| int   | 200     | Max results (max 500)                |
| `offset`| int  | 0       | Pagination offset                    |

**Response:**
```json
[
  {
    "jid": "919876543210@s.whatsapp.net",
    "name": "John Doe",
    "isGroup": false,
    "unreadCount": 3,
    "lastMessageTs": 1724847000,
    "lastMessage": "Hey, are you coming?"
  },
  {
    "jid": "120363045678901234@g.us",
    "name": "Family Group",
    "isGroup": true,
    "unreadCount": 0,
    "lastMessageTs": 1724846500,
    "lastMessage": "Dinner at 8"
  }
]
```

By default, only saved contacts and groups are returned. Pass `?all=1` to include unsaved numbers.

---

#### `GET /chats/:jid`

Single chat detail with contact info and presence.

**Response:**
```json
{
  "jid": "919876543210@s.whatsapp.net",
  "name": "John Doe",
  "isGroup": false,
  "unreadCount": 3,
  "lastMessageTs": 1724847000,
  "lastMessage": "Hey, are you coming?",
  "messageCount": 247,
  "contact": {
    "name": "John Doe",
    "notify": "John",
    "phoneNumber": "919876543210",
    "isSaved": true
  },
  "presence": {
    "status": "composing",
    "lastSeen": null,
    "updatedAt": 1724847005000
  }
}
```

---

#### `POST /chats/:jid/read`

Mark a chat as read. Resets unread count to 0 and sends read receipts to WhatsApp.

**Response:**
```json
{ "ok": true }
```

---

### Messages

#### `GET /messages/:jid`

Get messages for a chat. Automatically marks the chat as read.

| Param    | Type | Default | Description                        |
|----------|------|---------|------------------------------------|
| `limit`  | int  | 100     | Max messages (max 500)             |
| `before` | int  | —       | Unix timestamp — get messages before this time |

**Response:**
```json
[
  {
    "id": "3EB0A1B2C3D4E5F6",
    "fromMe": false,
    "senderJid": "919876543210@s.whatsapp.net",
    "senderName": "John Doe",
    "timestamp": 1724847000,
    "text": "Hey, are you coming?",
    "mediaType": null,
    "hasMedia": false,
    "mediaUrl": null,
    "status": "delivered"
  },
  {
    "id": "4FC0B2C3D4E5F6A7",
    "fromMe": true,
    "senderJid": "919876543210:42@s.whatsapp.net",
    "senderName": "You",
    "timestamp": 1724847005,
    "text": "",
    "mediaType": "image",
    "hasMedia": true,
    "mediaUrl": "/messages/919876543210%40s.whatsapp.net/4FC0B2C3D4E5F6A7/media",
    "status": "read"
  }
]
```

**Message status values:** `error` | `pending` | `sent` | `delivered` | `read`

**Media types:** `image` | `video` | `audio` | `document` | `sticker` | `null`

---

### Media

#### `GET /messages/:jid/:msgId/media`

Download media for a message. Returns the file directly (not JSON).

| Param  | Type | Description                    |
|--------|------|--------------------------------|
| `thumb`| `1`  | Return thumbnail instead of full media |

**Response:** The raw file with appropriate `Content-Type`:
- Images: `image/jpeg`, `image/png`, `image/webp`
- Video: `video/mp4`
- Audio: `audio/ogg`
- Documents: `application/octet-stream`

**Caching:** Media files have `Cache-Control: public, max-age=604800` (7 days). Thumbnails have `max-age=86400` (1 day).

**Fallback chain:**
1. Try to serve from local disk cache
2. Try to download from WhatsApp (if message is recent)
3. Try to serve thumbnail (`jpegThumbnail`)
4. Return 404

---

### Send

#### `POST /send`

Send a text message or base64-encoded media.

**Content-Type:** `application/json`

**Send text:**
```json
{
  "jid": "919876543210@s.whatsapp.net",
  "text": "Hello from the server!"
}
```

**Send media (base64):**
```json
{
  "jid": "919876543210@s.whatsapp.net",
  "mediaType": "image",
  "mediaBase64": "/9j/4AAQSkZJRg...",
  "caption": "Check this out!",
  "mimetype": "image/jpeg"
}
```

| Field        | Type   | Required | Description                                      |
|--------------|--------|----------|--------------------------------------------------|
| `jid`        | string | yes      | Recipient JID (see [JID Format](#jid-format))    |
| `text`       | string | no*      | Text message                                     |
| `mediaType`  | string | no*      | `image`, `video`, `audio`, `document`            |
| `mediaBase64`| string | no*      | Base64-encoded file                              |
| `caption`    | string | no       | Caption for media                                |
| `mimetype`   | string | no       | MIME type (e.g. `image/jpeg`)                    |
| `fileName`   | string | no       | Filename for documents                           |

*Either `text` OR `mediaBase64` + `mediaType` is required.

**Response:**
```json
{
  "ok": true,
  "id": "5AD0C3D4E5F6A7B8",
  "timestamp": 1724847010
}
```

---

#### `POST /send-media`

Send media via multipart form upload. Better for large files than base64.

**Content-Type:** `multipart/form-data`

| Field       | Type   | Required | Description                           |
|-------------|--------|----------|---------------------------------------|
| `jid`       | string | yes      | Recipient JID                         |
| `file`      | file   | yes      | The file to send (max 16 MB)          |
| `caption`   | string | no       | Caption for images/videos             |
| `mediaType` | string | no       | Override auto-detection               |

Media type is auto-detected from the file's MIME type if not specified.

**Response:**
```json
{
  "ok": true,
  "id": "6BE0D4E5F6A7B8C9",
  "timestamp": 1724847015
}
```

**Example (curl):**
```bash
curl -X POST https://your-app.onrender.com/send-media \
  -F "jid=919876543210@s.whatsapp.net" \
  -F "file=@photo.jpg" \
  -F "caption=Look at this!"
```

---

### Presence

#### `GET /presence/:jid`

Get online/typing status for a contact or group.

**Individual chat response:**
```json
{
  "status": "composing",
  "lastSeen": null,
  "updatedAt": 1724847005000
}
```

**Group chat response** (all participants):
```json
{
  "919876543210@s.whatsapp.net": {
    "status": "available",
    "lastSeen": null,
    "updatedAt": 1724847000000
  },
  "919876543211@s.whatsapp.net": {
    "status": "composing",
    "lastSeen": null,
    "updatedAt": 1724847005000
  }
}
```

**Status values:**

| Status       | Meaning                                      |
|--------------|----------------------------------------------|
| `available`  | Online — WhatsApp is open and foregrounded   |
| `composing`  | Typing a message                             |
| `recording`  | Recording a voice note                       |
| `paused`     | Stopped typing or recording                  |
| `unavailable`| Offline (may include `lastSeen` timestamp)   |

Presence is tracked in memory and expires after 5 minutes of no update. Poll this endpoint every 3-5 seconds while viewing a chat.

---

#### `GET /presence`

Get ALL currently active presences (non-expired, non-unavailable).

**Response:**
```json
{
  "919876543210@s.whatsapp.net": {
    "status": "available",
    "lastSeen": null,
    "updatedAt": 1724847000000
  },
  "120363045678901234@g.us:919876543211@s.whatsapp.net": {
    "status": "composing",
    "lastSeen": null,
    "updatedAt": 1724847005000
  }
}
```

Group participant keys use the format `<groupJid>:<participantJid>`.

---

### Contacts & Profile

#### `GET /contacts`

List all known contacts (synced from WhatsApp + push names from messages).

**Response:**
```json
[
  {
    "jid": "919876543210@s.whatsapp.net",
    "name": "John Doe",
    "notify": "John",
    "phoneNumber": "919876543210",
    "isSaved": true
  },
  {
    "jid": "919876543211@s.whatsapp.net",
    "name": "919876543211",
    "notify": "Jane",
    "phoneNumber": "",
    "isSaved": false
  }
]
```

---

#### `GET /profile-pic/:jid`

Get profile picture as JPEG.

**Response:** `image/jpeg` with `Cache-Control: public, max-age=3600`

Returns 404 if the user has no profile picture.

---

## Data Models

### Chat

| Field           | Type    | Description                          |
|-----------------|---------|--------------------------------------|
| `jid`           | string  | WhatsApp JID (unique identifier)     |
| `name`          | string  | Display name (contact name or group subject) |
| `isGroup`       | boolean | Whether this is a group chat         |
| `unreadCount`   | number  | Number of unread messages            |
| `lastMessageTs` | number  | Unix timestamp of last message       |
| `lastMessage`   | string  | Text preview of last message         |

### Message

| Field       | Type    | Description                              |
|-------------|---------|------------------------------------------|
| `id`        | string  | WhatsApp message ID (unique per chat)    |
| `fromMe`    | boolean | Whether you sent this message            |
| `senderJid` | string  | JID of the sender (useful in groups)     |
| `senderName`| string  | Display name of the sender               |
| `timestamp` | number  | Unix timestamp                           |
| `text`      | string  | Message text (or caption for media)      |
| `mediaType` | string  | `image`, `video`, `audio`, `document`, `sticker`, or `null` |
| `hasMedia`  | boolean | Whether this message has downloadable media |
| `mediaUrl`  | string  | API path to download media (null if no media) |
| `status`    | string  | `error`, `pending`, `sent`, `delivered`, `read` |

### Contact

| Field         | Type    | Description                          |
|---------------|---------|--------------------------------------|
| `jid`         | string  | WhatsApp JID                         |
| `name`        | string  | Saved contact name                   |
| `notify`      | string  | Display name from WhatsApp (pushName)|
| `phoneNumber` | string  | Phone number                         |
| `isSaved`     | boolean | Whether this is a saved contact      |

### Presence

| Field       | Type   | Description                               |
|-------------|--------|-------------------------------------------|
| `status`    | string | `available`, `composing`, `recording`, `paused`, `unavailable` |
| `lastSeen`  | number | Unix timestamp when user went offline (null if online) |
| `updatedAt` | number | Unix timestamp of last presence update    |

---

## JID Format

WhatsApp uses JIDs (Jabber IDs) to identify contacts and groups.

| Type              | Format                          | Example                          |
|-------------------|---------------------------------|----------------------------------|
| Individual        | `<phone>@s.whatsapp.net`       | `919876543210@s.whatsapp.net`    |
| Group             | `<id>@g.us`                    | `120363045678901234@g.us`        |
| Group participant | `<phone>@s.whatsapp.net`       | `919876543210@s.whatsapp.net`    |

The API accepts phone numbers without the `@s.whatsapp.net` suffix — it's added automatically. So you can send `919876543210` instead of `919876543210@s.whatsapp.net`.

---

## Persistence

| Data                  | Storage           | Survives restart | Survives redeploy |
|-----------------------|-------------------|------------------|-------------------|
| Chats                 | PostgreSQL        | ✅ Yes           | ✅ Yes            |
| Contacts              | PostgreSQL        | ✅ Yes           | ✅ Yes            |
| Messages              | PostgreSQL        | ✅ Yes           | ✅ Yes            |
| Unread counts         | PostgreSQL        | ✅ Yes           | ✅ Yes            |
| Message status        | PostgreSQL        | ✅ Yes           | ✅ Yes            |
| WhatsApp auth state   | Local filesystem  | ✅ Yes           | ❌ No — re-scan QR|
| Media cache           | Local filesystem  | ✅ Yes           | ❌ No — re-downloads on demand |
| Presence (online/typing)| In-memory       | ❌ No            | ❌ No             |

After a redeploy, you need to re-scan the QR code. Chat history is preserved in PostgreSQL — Baileys history sync fills in any new messages on top.

---

## Environment Variables

| Variable       | Required | Default         | Description                                      |
|----------------|----------|-----------------|--------------------------------------------------|
| `DATABASE_URL` | yes      | —               | PostgreSQL connection string                     |
| `PORT`         | no       | `3000`          | Server port (Render sets this automatically)     |
| `AUTH_DIR`     | no       | `./auth_state`  | Where Baileys stores WhatsApp credentials        |
| `MEDIA_DIR`    | no       | `./data/media`  | Where downloaded media is cached                 |
| `API_KEY`      | no       | —               | API key for auth (send as `X-API-Key` header)    |
| `LOG_LEVEL`    | no       | `silent`        | Pino log level: `silent` `fatal` `error` `warn` `info` `debug` `trace` |

---

## Project Structure

```
├── index.js         # Express server, routes, boot sequence
├── wa.js            # WhatsApp connection, event handlers, presence tracking
├── db.js            # PostgreSQL schema, queries, migrations
├── media.js         # Media download, disk cache, thumbnails
├── helpers.js       # Timestamp parsing, message unwrapping, safeJSON, JID utils
├── render.yaml      # Render deployment blueprint
├── package.json
├── .env.example
├── .gitignore
└── README.md
```

### Module Responsibilities

**`index.js`** — HTTP layer. Defines all routes, handles request/response, starts the server. No WhatsApp or database logic — delegates everything to `wa.js` and `db.js`.

**`wa.js`** — WhatsApp layer. Manages the Baileys socket connection, handles all WhatsApp events (messages, contacts, chats, presence), provides `sendText()`, `sendMedia()`, `markChatRead()` API functions.

**`db.js`** — Database layer. PostgreSQL schema with migrations, all CRUD operations for chats/contacts/messages. Every function is async.

**`media.js`** — Media layer. Downloads media from WhatsApp to disk, serves from cache, generates thumbnails, handles expired media fallback.

**`helpers.js`** — Pure utility functions. No side effects, no state. Timestamp parsing, message content extraction, JID normalization, ASCII-safe JSON serialization.

---

## How It Works

### Connection Lifecycle

1. Server starts → loads auth state from `AUTH_DIR`
2. Baileys opens WebSocket to WhatsApp servers
3. If no auth exists → QR code is generated → user scans
4. Once authenticated → `messaging-history.set` fires with all chats/contacts/messages
5. Server stores everything in PostgreSQL
6. Real-time events (new messages, presence, read receipts) stream in via WebSocket

### Message Flow

**Incoming message:**
```
WhatsApp → Baileys WebSocket → messages.upsert event
  → recordMessage() → PostgreSQL
  → if media: cacheRawMsg() → downloadMediaBackground() → disk
```

**Outgoing message:**
```
Client → POST /send → wa.sendText() → Baileys.sendMessage()
  → recordMessage() → PostgreSQL
  → WhatsApp delivery → messages.update event → status updated in PostgreSQL
```

### Presence Flow

```
WhatsApp → Baileys WebSocket → presence.update event
  → updatePresence() → in-memory Map
  → Client polls GET /presence/:jid → returns current status
```

### Media Flow

**Download (incoming):**
```
Message arrives with hasMedia=true
  → rawMsg cached in memory (24h TTL)
  → background: downloadMediaMessage() → write to disk → update PostgreSQL
  → Client requests GET /messages/:jid/:msgId/media
  → serve from disk (or download on-demand if not cached yet)
```

**Upload (outgoing):**
```
Client → POST /send-media (multipart) or POST /send (base64)
  → wa.sendMedia() → Baileys.sendMessage()
  → recordMessage() → PostgreSQL
```

---

## Limitations

| Limitation | Details |
|---|---|
| **One WhatsApp account per server** | Baileys connects one account. Run multiple instances for multiple accounts. |
| **Media is ephemeral on Render free** | Media cache is lost on redeploys. Re-downloads on demand from WhatsApp (fails for media >30 days old). |
| **Presence is in-memory only** | Online/typing status is lost on restart. Not persisted to database. |
| **No webhook/push** | Clients must poll for new messages. No WebSocket or webhook push to clients. |
| **WhatsApp rate limits** | Sending too many messages too fast may get the account temporarily banned. |
| **History sync is one-time** | Baileys syncs full history on first connect. After that, only new messages arrive in real-time. |
| **No end-to-end encryption bypass** | Baileys operates as a normal WhatsApp client. Messages are E2E encrypted between your phone and the recipient. |

---

## License

MIT
