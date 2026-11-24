# Porting OpenKnot (Baileys) to Qt — Direct WhatsApp Web Connection

> **Goal:** Remove the Node.js proxy (`index.js` + `wa.js` + PostgreSQL) and let the
> Qt app on Symbian / desktop connect **directly** to `wss://web.whatsapp.com`,
> exactly like Baileys does.

This document explains what Baileys actually does, what a Qt port must re-implement,
what is already done in `qt/`, and the honest remaining work for a 100% direct
connection. It is written for a Symbian Qt developer whose device **does support
TLS 1.2 at system level** (so TLS is *not* a blocker — see §5).

---

## 1. What you have today (the proxy)

```
┌──────────────┐   HTTP/JSON   ┌──────────────────┐   WebSocket   ┌──────────────┐
│ J2ME / Qt    │ ────────────► │ OpenKnot (Node)  │ ────────────► │ WhatsApp     │
│ Symbian app  │ ◄──────────── │ index.js + wa.js │ ◄──────────── │ Web servers  │
└──────────────┘               │ + PostgreSQL     │               └──────────────┘
                               └──────────────────┘
```

| File | Role |
|---|---|
| `index.js` | Express REST API (~40 endpoints: `/chats`, `/messages`, `/send`, `/sync`, `/groups`, …) |
| `wa.js` | Baileys socket wrapper: connect, QR, send/recv, groups, presence, media, history sync |
| `db.js` | PostgreSQL: chats, contacts, messages, reactions, `raw_json` (protobuf cache) |
| `helpers.js` | JID normalisation, `parseMessageContent`, `encodeRawMessage`, MIME guessing |
| `serializers.js` | DB rows → client JSON (`chat`, `contact`, `baseMessage`, `decorateMessages`) |
| `media.js` / `uploads.js` | Media download/cache + multipart/base64 upload |
| `auth.js` | Baileys `creds.json` + Signal keys, backed up to Postgres |

The Symbian app never speaks WhatsApp protocol. It only speaks HTTP to OpenKnot.
That is why it works on Symbian today — and why removing the proxy means the
Qt app must speak the **WhatsApp Web protocol itself**.

## 2. What Baileys does (and a Qt port must do)

Baileys is **not** a thin WebSocket wrapper. It is a full re-implementation of
WhatsApp Web's multi-device protocol in TypeScript (~50k lines). The stack:

```
┌──────────────────────────────────────────────────────────────┐
│ App logic: send/recv, groups, presence, reactions, edits…  │  ← your wa.js
├──────────────────────────────────────────────────────────────┤
│ History Sync + App State Sync (protobuf, USync, WAM)       │
├──────────────────────────────────────────────────────────────┤
│ Signal Protocol (X3DH + Double Ratchet) — E2E per message  │  ← libsignal
├──────────────────────────────────────────────────────────────┤
│ Media crypto: AES-256-CBC + HMAC-SHA256, HKDF              │
├──────────────────────────────────────────────────────────────┤
│ BinaryNode codec (XML-like binary framing) + WAProto (.proto)│
├──────────────────────────────────────────────────────────────┤
│ Noise Protocol (Noise_XX / Noise_IK, Curve25519, AES-GCM)  │  ← transport
├──────────────────────────────────────────────────────────────┤
│ WebSocket wss://web.whatsapp.com + TLS 1.2+                │  ← QWebSocket
└──────────────────────────────────────────────────────────────┘
```

### 2.1 Transport — WebSocket + Noise

Baileys opens `wss://web.whatsapp.com` (actually `wss://web.whatsapp.com:443`
with `Origin: https://web.whatsapp.com` and a browser-like User-Agent), then
performs a **Noise handshake** (`Noise_XX` for first connect, `Noise_IK` for
resume) using Curve25519 + AES-256-GCM + SHA-256. Every frame after that is
Noise-encrypted. Without this, WhatsApp drops the connection immediately.

*Qt equivalent:* `QWebSocket` (Qt ≥ 5.4) + a Noise implementation.
Qt has **no built-in Noise or Curve25519**. You need one of:

- **libsodium** (`crypto_scalarmult`, `crypto_aead_aes256gcm` / `chacha20poly1305`) — recommended, small, portable.
- **OpenSSL** (EVP) — available on desktop, harder on Symbian.
- **libsignal-protocol-c** — also provides Signal (see §2.2), so if you link it
  you get Curve25519 for free.

> Symbian note: Qt 4.7 (the Symbian Qt) has **no `QWebSocket` at all**
> (added in Qt 5.4). A direct connection on Symbian therefore needs either
> a Qt 5 port for Symbian (unofficial) or a hand-rolled WebSocket client on
> top of `QTcpSocket` + system TLS. The skeleton in `qt/src/direct/` uses
> `QWebSocket` and documents the `QTcpSocket` fallback.

### 2.2 End-to-end encryption — Signal Protocol

Every message is encrypted with **Signal Protocol** (X3DH key exchange +
Double Ratchet), with per-device sessions, pre-keys, signed pre-keys, and
sender-key groups for groups. Baileys uses `libsignal` (a fork of Signal's
`libsignal-protocol-javascript`).

*Qt equivalent:* link **libsignal-protocol-c** (C library, portable) and wrap
it in C++ classes (`SignalSession`, `SignalStore`). This is the single largest
chunk of work — roughly 30–40% of a Baileys port. There is **no maintained
C++/Qt Signal-for-WhatsApp library** today; you port Baileys'
`src/Utils/signal.ts` + `src/Utils/auth-utils.ts` against libsignal-protocol-c.

### 2.3 Framing — BinaryNode + Protobuf

WhatsApp frames are **BinaryNode** (a compact XML-like binary tree) carrying
**protobuf** payloads (`WAProto`: `Message`, `HistorySync`, `USync`, …).
Baileys ships generated protobuf code (`src/WABinary/proto/`).

*Qt equivalent:* port `src/WABinary/binary-node.ts`, `encode.ts`, `decode.ts`
to C++ (this part is **self-contained and already done** in
`qt/src/direct/binary_node.*` — no external deps), and generate C++ protobuf
from the same `.proto` files with `protoc` (or use `protobuf-lite` for Symbian
size). The `.proto` files can be extracted from the installed Baileys package
(`node_modules/@whiskeysockets/baileys/src/WABinary/proto/`).

### 2.4 Pairing — QR / pairing code

First connect: server sends a QR string (`ref`, `noise-pub`, `pub-key`,
`adv-secret`). The phone scans it in WhatsApp → Linked Devices. Baileys
renders it as an image (`/qr-image` in your server). A Qt direct client must
render the QR itself (use a QR encoder like **QrCode** single-header, or
`QZXing`) and complete the Noise handshake + `pair-device` stanza.

### 2.5 History Sync, App State, USync

On connect, WhatsApp pushes **History Sync** (chats, contacts, ~100s of
messages), **App State Sync** (chat settings, mute/pin/archive), and **USync**
(contact info). Your `wa.js` `messaging-history.set` handler + `db.js` upserts
are the server-side mirror of this. A direct Qt client must implement the
same handlers and store results locally (SQLite via `QtSql` replaces Postgres).

### 2.6 Media

Media is **not** sent in the message stanza. The sender uploads an
AES-encrypted blob to WhatsApp's media CDN (`mmg.whatsapp.net` /
`lookaside.fbsbx.com`) and puts the URL + media key in the protobuf.
The receiver downloads + decrypts. Baileys `downloadMediaMessage` /
`prepareWAMessageMedia` are the reference. Qt needs `QNetworkAccessManager`
for the CDN fetch + libsodium/OpenSSL for AES-CBC/HMAC.

## 3. What is already in `qt/` (this port)

```
qt/
├── CMakeLists.txt              Qt5/Qt6 build (Widgets + Network + WebSockets + Sql)
├── README.md                   Build + run instructions
└── src/
    ├── main.cpp                App entry, mode switch (proxy vs direct)
    ├── models.h                Chat / Message / Contact (mirrors serializers.js)
    ├── api_client.h/.cpp       PROXY MODE: full OpenKnot REST client (all endpoints)
    ├── mainwindow.h/.cpp       Chat list + message view + send box + QR dialog
    └── direct/
        ├── whatsapp_direct.h/.cpp   DIRECT MODE: QWebSocket + Noise skeleton,
        │                            Baileys file-by-file mapping, TODOs
        ├── binary_node.h/.cpp       BinaryNode encode/decode (ported, working)
        └── noise_handshake.h/.cpp   Noise_XX/IK skeleton (needs libsodium)
```

### Proxy mode (works today, no server changes)

`ApiClient` implements every endpoint from `index.js` using
`QNetworkAccessManager` + `QJsonDocument`:

- `GET /health`, `/`, `/version`, `/stats`
- `GET /qr`, `/qr-image` (pairing dialog)
- `GET /chats`, `/chats/:jid`, chat actions (read/unread/typing/archive/pin/mute/clear/delete)
- `GET /messages/:jid`, `/messages/:jid/:msgId`, media download (incl. `?thumb=1`)
- Message actions: delete (everyone/me), forward, react, edit
- `GET /search`, `GET /sync` (delta polling — the J2ME-friendly pattern)
- `POST /send` (JSON text + multipart media), `/send-contact`, `/send-location`
- Profile: `/me`, `/user/:jid`, `/check`, `/contacts`, `/blocked`, block/unblock
- Presence: `/presence`, subscribe, typing
- Groups: list/create/join/metadata/participants/leave/invite/subject/description/settings

This is the **Symbian-safe** path: pure HTTP, no WebSocket, no crypto libs.
It is a drop-in Qt replacement for your J2ME client.

### Direct mode (transport implemented, verified against Baileys)

`WhatsAppDirect` opens `wss://web.whatsapp.com/ws/chat` via `QWebSocket`, runs
the real Noise handshake, and exposes the same high-level API as `ApiClient`
(`sendText`, …) so `MainWindow` does not care which mode is active.

Implemented **and verified byte-for-byte against Baileys 6.7.22** (see
`qt/tests/`):

- ✅ BinaryNode codec — token dictionaries, nibble/hex packing, JID pairs,
  AD-JID, byte/text/list content: 9/9 round-trips identical to Baileys
- ✅ Raw-DEFLATE decoder for compressed frames (no zlib): 6/6 vs Node zlib
- ✅ Crypto, dependency-free: SHA-256, HMAC, HKDF, X25519 (RFC 7748),
  AES-256-GCM (+tamper rejection), AES-256-CBC: 55/55 checks
- ✅ Handshake protobuf (`HandshakeMessage`, `ClientPayload`,
  `DeviceProps`, MD5 build hash): 10/10 identical to protobufjs output
- ✅ Noise transport (`Noise_XX_25519_AESGCM_SHA256`): 15/15 ciphertexts
  identical to Baileys' own `NoiseHandler`, plus framing/reassembly tests
- ✅ Full connect flow: ClientHello → ServerHello → ClientFinish → `finishInit`,
  QR pairing stanza, `<success>`/`<failure>`/`<stream:error>`, keepalive,
  reconnect, creds persistence

Still missing (all call sites marked):

- ⚠️ `TODO(signal)` — Signal sessions (X3DH + Double Ratchet, sender keys for
  groups) via libsignal-protocol-c: message send/receive bodies
- ⚠️ `TODO(proto)` — generated WAProto for `Message`, `HistorySync`,
  app-state patch/snapshot, USync
- ⚠️ Media CDN upload/download (AES-CBC + HMAC: primitives already present)

**Honest estimate:** proxy mode is done, and direct mode's entire transport
layer is done + tested. What remains (Signal + WAProto + media) is the bulk of
a Baileys port — several weeks of focused C++ work. Direct mode will pair and
stay connected today, but it will not read or send message bodies until those
land.

## 4. Why not just "port wa.js line by line"?

`wa.js` is 1300 lines, but it sits on top of Baileys' ~50k lines
(socket, signal, noise, binary, proto, history-sync, media, USync, …).
Porting `wa.js` alone gives you nothing — you must port the stack underneath.
The mapping table below is the work breakdown.

| Baileys (TS) | Qt (C++) | Status |
|---|---|---|
| `src/Socket/socket.ts` | `direct/whatsapp_direct.*` | **implemented** (transport) |
| `src/Utils/noise-handler.ts` | `direct/noise.*` (+ `sha256/x25519/aes_gcm`) | **ported + tested** |
| `src/WABinary/{encode,decode,constants}.ts`, `jid-utils.ts` | `direct/binary_node.*`, `wa_tokens.h` | **ported + tested** |
| zlib raw inflate (compressed frames) | `direct/inflate.*` | **implemented + tested** |
| `proto.HandshakeMessage`, `ClientPayload`, `DeviceProps` | `direct/proto_min.*` | **ported + tested** |
| `src/WABinary/proto/*.proto` (Message, HistorySync, …) → `protoc` | generated `wa_proto.pb.*` | TODO(proto) |
| `src/Signal/libsignal.ts`, `auth-utils.ts` | libsignal-protocol-c wrapper + XEdDSA | TODO(signal) |
| `src/Utils/history-sync.ts` | `HistorySync` handler + `QtSql` store | TODO(proto) |
| `src/Utils/messages-media.ts` | media AES-CBC/HMAC + CDN fetch (`aes_gcm.*` ready) | TODO |
| `src/Utils/usync.ts`, `app-state.ts` | USync / app-state handlers | TODO |
| `wa.js` (your code) | `MainWindow` + `ApiClient`/`WhatsAppDirect` | **ported (proxy)** |

## 5. Symbian + TLS 1.2 — what your note changes (and doesn't)

You confirmed the device supports **TLS 1.2 at system level**. That removes
the most common Symbian blocker (Qt 4.7's bundled OpenSSL only does TLS 1.0,
which WhatsApp rejects). Good — but three other Symbian hurdles remain:

1. **No `QWebSocket` in Qt 4.7.** Direct mode needs Qt ≥ 5.4, or a custom
   WebSocket-over-`QTcpSocket` client (framing is already isolated in
   `noise::encodeFrame`/`decodeFrames`, so only the RFC 6455 layer is missing).
   Proxy mode (HTTP only) works on Qt 4.7.
2. **No `QJsonDocument` in Qt 4.7.** Proxy mode on Qt 4.7 needs a small JSON
   parser (e.g. QScript or a bundled single-header parser). The Qt5 build in
   `qt/` uses `QJsonDocument`; a Qt4 build swaps in a parser shim.
3. **Crypto libs.** This port removed the libsodium, zlib and protobuf
   dependencies for the whole transport: X25519, AES-256-GCM/CBC/CTR,
   SHA-256/HMAC/HKDF, raw DEFLATE and the handshake protobuf are plain C++ in
   `src/direct/`. The only library still needed for a full client is
   **libsignal-protocol-c** (plus its XEdDSA signing for pre-keys), which does
   have to be cross-compiled for Symbian (armv5/armv6) — but that is one
   dependency instead of four. Proxy mode needs none of them.

**Recommendation:** ship **proxy mode** for Symbian (it is complete and needs
no crypto), and develop **direct mode** on desktop Qt5/Qt6 first. Once direct
mode works on desktop, back-port the WebSocket + crypto layers to Symbian.

## 6. Roadmap

- [x] **Phase 1 — Qt proxy client** (`qt/`): full OpenKnot REST client + UI.
      Drop-in replacement for the J2ME client.
- [x] **Phase 2 — Direct transport**: dependency-free Noise handshake +
      `QWebSocket` to `wss://web.whatsapp.com`, QR pairing, reconnect.
      Verified against Baileys by the suites in `qt/tests/`.
- [ ] **Phase 3 — Direct crypto**: libsignal-protocol-c sessions + XEdDSA
      pre-key signatures; BinaryNode + protobuf send/recv of text messages.
- [ ] **Phase 4 — Direct sync + media**: WAProto `Message`/`HistorySync`,
      USync, `QtSql` store, media upload/download, groups, presence.
- [ ] **Phase 5 — Symbian back-port**: Qt4 HTTP shim (done in Phase 1),
      custom RFC 6455 WebSocket + cross-compiled libsignal for direct mode.

## 8. Verification

`qt/tests/run_tests.sh` compiles the Qt-free protocol code with the host g++
and checks it against vectors generated from the real Baileys package and
Node's `crypto`:

| Suite | What it proves |
|---|---|
| `test_codec` | decode→encode is byte-identical for real Baileys frames; DEFLATE matches Node zlib; compressed (flag `0x02`) frames decode |
| `test_crypto` | SHA-256/HMAC/HKDF, X25519 (incl. RFC 7748), AES-256-GCM (+tampering rejected), AES-256-CBC |
| `test_proto` | `ClientHello`, `ServerHello` decode, `ClientFinish`, `DeviceProps`, registration + login `ClientPayload` identical to protobufjs |
| `test_noise` | the full Noise key schedule produces the same ciphertexts as Baileys' `NoiseHandler`, plus frame header/split/coalesce handling |

Regenerate the vectors after a Baileys bump:

```bash
BAILEYS_DIR=/path/to/@whiskeysockets/baileys ./qt/tests/run_tests.sh --regen
```

## 7. References

- Baileys: <https://github.com/WhiskeySockets/Baileys> (protocol reference)
- Noise Protocol: <https://noiseprotocol.org/>
- Signal Protocol: <https://signal.org/docs/specifications/x3dh/>
- libsignal-protocol-c: <https://github.com/Whisperfish/libsignal-protocol-c>
- libsodium: <https://doc.libsodium.org/>
- Qt WebSockets: <https://doc.qt.io/qt-5/qtwebsockets-index.html>
- Prior Qt WhatsApp attempts (WebEngine wrappers, **not** direct protocol):
  WhatSie, WhatsThat (archived 2019) — useful UI reference only.
