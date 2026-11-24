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
│ WebSocket wss://web.whatsapp.com + TLS 1.2+                │  ← qt4compat
└──────────────────────────────────────────────────────────────┘
```

### 2.1 Transport — WebSocket + Noise

Baileys opens `wss://web.whatsapp.com` (actually `wss://web.whatsapp.com:443`
with `Origin: https://web.whatsapp.com` and a browser-like User-Agent), then
performs a **Noise handshake** (`Noise_XX` for first connect, `Noise_IK` for
resume) using Curve25519 + AES-256-GCM + SHA-256. Every frame after that is
Noise-encrypted. Without this, WhatsApp drops the connection immediately.

*Qt equivalent:* a WebSocket client + a Noise implementation. Neither is a
problem any more:

- **WebSocket** — Qt only grew `QWebSocket` in 5.4, so `qt/src/qt4compat/
  websocket.cpp` implements RFC 6455 directly on `QSslSocket` (HTTP upgrade
  with `Origin`/`User-Agent`, masked client frames, fragmentation, ping/pong,
  close, `Sec-WebSocket-Accept` verification). It works on Qt 4.7 and Qt 5/6.
- **Noise + Curve25519 + AES-GCM + SHA-256** — hand-written in
  `qt/src/direct/` (`noise.cpp`, `x25519.cpp`, `aes_gcm.cpp`, `sha256.cpp`)
  and verified against Baileys' own `NoiseHandler`. No libsodium, no OpenSSL,
  no Qt crypto module.

> Symbian note: the whole stack therefore builds on stock Qt 4.7.4 —
> `qt/tests/qt4_check.sh` compiles every Qt source against the real 4.7.4
> headers to keep it that way.

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
├── CMakeLists.txt              Qt4-first build (Core + Gui + Network; Qt5/6 also work)
├── README.md                   Build + run instructions
└── src/
    ├── main.cpp                App entry, mode switch (proxy vs direct)
    ├── models.h                Chat / Message / Contact (mirrors serializers.js)
    ├── api_client.h/.cpp       PROXY MODE: full OpenKnot REST client (all endpoints)
    ├── mainwindow.h/.cpp       Chat list + message view + send box + QR dialog
    ├── qt4compat/              Qt 4.7 back-port (JSON, RFC 6455 socket, SignalHook)
    └── direct/
        ├── whatsapp_direct.h/.cpp   DIRECT MODE: socket lifecycle, stanzas, pairing
        ├── binary_node.h/.cpp       BinaryNode encode/decode (ported, working)
        └── noise.*, signal.*, …     Noise + Signal + WAProto, all dependency-free
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

`WhatsAppDirect` opens `wss://web.whatsapp.com/ws/chat` (RFC 6455 over
`QSslSocket` — see §5), runs the real Noise handshake, and exposes the same
high-level API as `ApiClient` (`sendText`, `sendMedia`, `downloadMedia`, …) so
`MainWindow` does not care which mode is active.

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
- ✅ **Signal Protocol, hand-written** (`src/direct/signal.cpp`): X3DH, Double
  Ratchet, WhisperTextProtocol messages — interops with libsignal in *both*
  directions (18/18 + 16/16 checks; libsignal decrypts our ciphertext and we
  decrypt its replies)
- ✅ **XEdDSA pre-key signatures + Ed25519** (`src/direct/ed25519.cpp`):
  31/31 against curve25519-js / Node crypto
- ✅ **WAProto subset** (`src/direct/wa_proto.cpp`): `Message`,
  `WebMessageInfo`, `HistorySync`, `HistorySyncNotification`, `MessageKey`,
  `ContextInfo`, media metadata, `deviceSentMessage` — encode **and** decode
  byte-identical to protobufjs (58/58)
- ✅ **Media / history crypto** (`src/direct/media_crypto.cpp`): HKDF
  `"WhatsApp <Type> Keys"` → AES-256-CBC + 10-byte HMAC, and a zlib wrapper on
  the DEFLATE decoder: 59/59, including a real encrypted `HistorySync` blob
- ✅ **Message layer** (`src/direct/wa_message.cpp`): pre-key bundle parsing
  from the real `<iq xmlns=encrypt>` node, per-device `pkmsg`/`msg`
  encryption, `<participants>` assembly, `deviceSentMessage` wrapping, and
  incoming decrypt + routing (18/18 decrypt checks, 29/29 build checks, with
  libsignal on the other end)
- ✅ **Send + history wired into the app**: one-time pre-key generation and
  upload, device-bundle fetch + 5-minute cache, text send, receipts, and
  `HistorySync` download → decrypt → inflate → parse → chat list
- ✅ **Groups** (`src/direct/sender_key.cpp`,`wa_message.cpp`): `<iq
  xmlns=w:g2><query/></iq>` metadata, one sender-key chain per (group, member),
  `HKDF("WhisperGroup")` message keys, AES-256-CBC bodies signed with XEdDSA,
  `SenderKeyDistributionMessage` fanned out to devices that lack it
  (sender-key memory), skipped-key caching on receive (2000-message window),
  chains persisted in `creds.json`. Baileys' own `GroupCipher` decrypts our
  `skmsg` and we decrypt its (42 + 10 checks)
- ✅ **Media** (`src/direct/media_upload.cpp`): `<iq xmlns=w:m><media_conn/></iq>`,
  the exact CDN upload URL (base64url of `SHA-256(ciphertext‖MAC)`),
  `prepareUpload` (fresh mediaKey → HKDF → AES-256-CBC + 10-byte HMAC →
  `fileSha256`/`fileEncSha256`), every media template — image, video, audio,
  document, sticker (WebP → `StickerMessage`, same `/mms/image` path and Image
  key mapping) and voice notes (`ptt` + Ogg granule duration) — with
  `url`/`directPath`/`mediaKey`/`mediaKeyTimestamp`, and download →
  MAC-verify → decrypt → file. Baileys' `encryptedStream()` produces the same
  ciphertext and decrypts ours (89 + 12 checks)
- ✅ **Group administration**: create, join (code or chat.whatsapp.com link),
  leave, add/remove/promote/demote participants, subject, description, invite
  get/revoke — byte-shape identical to Baileys `groups.js`, exposed in the
  `Group` menu for both modes, with a metadata view (participants + roles). Incoming `w:gp2` notifications keep the chat
  list and cached metadata current
- ✅ **Qt 4.7.4**: every Qt API the client uses exists in 4.7.4 — verified by
  compiling all Qt sources against the real 4.7.4 headers
  (`qt/tests/qt4_check.sh`)
- ✅ **GCC 4.6.3** (the Symbian GCGE): the sources avoid member initialisers,
  `override`, `std::to_string`/`stoi` and `<chrono>`; `src/direct/compat.h`
  fills the gaps and `qt/tests/gcc46_check.sh` enforces it
- ✅ **STLport 5.2** (the Symbian STL): no `std::function` (`wa::function` in
  `src/direct/callable.h`), no smart pointers (refcounted `NodePtr`, raw
  pointers in `WhatsAppDirect`), no `initializer_list` (`compat::bytes`,
  `jsonObj`), no `<random>`/`<cstdint>`, no `std::move` (`compat::move`), no
  `vector::data()` (`compat::dataOf`, `Reader/Writer::bytes()`)

Still missing:

- ⚠️ App state (mute/pin/archive), presence privacy, USync device lists
  (bundles are fetched per send instead), media thumbnails/streaming.

**Honest estimate:** direct mode now sends and receives text (1:1 and groups),
uploads and downloads media, and loads chat history — all on Qt 4.7.4 API.

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
| `src/WABinary/proto/*.proto` (Message, HistorySync, …) → `protoc` | hand-written `wa_proto.cpp` subset | done (subset) |
| `src/Signal/libsignal.ts`, `auth-utils.ts` | `signal.cpp` (X3DH + ratchet) + `ed25519.cpp` (XEdDSA) | done (1:1) |
| `src/Utils/history-sync.ts` | `wa_history.cpp` + `media_crypto.cpp` | done |
| `src/Utils/messages-media.ts` | `media_crypto.*` + `media_upload.*` + CDN HTTP in `whatsapp_direct.cpp` | **done** |
| `src/Signal/Group/*` (sender keys) | `direct/sender_key.*` | **done + tested vs GroupCipher** |
| `src/Utils/usync.ts`, `app-state.ts` | USync / app-state handlers | TODO |
| `QWebSocket` (Qt 5.3) | `qt4compat/websocket.cpp` (RFC 6455 on `QSslSocket`) | **done** |
| `QJson*` (Qt 5.0) | `qt4compat/json.*` | **done** |
| `wa.js` (your code) | `MainWindow` + `ApiClient`/`WhatsAppDirect` | **ported (proxy)** |

## 5. Symbian + TLS 1.2 — what your note changes (and doesn't)

You confirmed the device supports **TLS 1.2 at system level**. That removes
the most common Symbian blocker (Qt 4.7's bundled OpenSSL only does TLS 1.0,
which WhatsApp rejects). Good — but three other Symbian hurdles remain:

1. ~~No `QWebSocket` in Qt 4.7.~~ Solved: `src/qt4compat/websocket.cpp` is an
   RFC 6455 client on `QSslSocket` (TLS, HTTP upgrade, masked frames,
   fragmentation, ping/pong, close, SHA-1 accept check). `QWebSocket` is no
   longer used anywhere.
2. ~~No `QJsonDocument` in Qt 4.7.~~ Solved: `src/qt4compat/json.{h,cpp}` is a
   Qt5-compatible QJson* implementation (parser, serialiser, COW values),
   tested by `qt/tests/test_qt4compat.cpp`. Other 4.8+/5 APIs that had crept
   in (`QHttpMultiPart`, `QUrlQuery`, `QCommandLineParser`, `QStringLiteral`,
   `toHtmlEscaped`, `…SecsSinceEpoch`, `QRandomGenerator`, functor `connect`)
   are gone too — see `qt/tests/qt4_check.sh`.
3. **Crypto libs.** This port removed the libsodium, zlib and protobuf
   dependencies for the whole transport: X25519, AES-256-GCM/CBC/CTR,
   SHA-256/HMAC/HKDF, raw DEFLATE and the handshake protobuf are plain C++ in
   `src/direct/`. The only library still needed for a full client is
   **libsignal-protocol-c** (plus its XEdDSA signing for pre-keys), which does
   have to be cross-compiled for Symbian (armv5/armv6) — but that is one
   dependency instead of four. Proxy mode needs none of them.

**Build:** `qt/openknot.pro` is the qmake project for the Symbian toolchain
(CMake is used on desktop; the sources are identical).

**Recommendation:** both modes now build against Qt 4.7.4 headers. Ship proxy
mode if the phone cannot do TLS 1.2 to `web.whatsapp.com`; direct mode otherwise
(set `OPENKNOT_INSECURE_TLS=1` only if the device root store is too old for
WhatsApp's chain).

## 6. Roadmap

- [x] **Phase 1 — Qt proxy client** (`qt/`): full OpenKnot REST client + UI.
      Drop-in replacement for the J2ME client.
- [x] **Phase 2 — Direct transport**: dependency-free Noise handshake +
      WebSocket to `wss://web.whatsapp.com`, QR pairing, reconnect.
      Verified against Baileys by the suites in `qt/tests/`.
- [x] **Phase 3 — Direct crypto**: hand-written Signal (X3DH + Double Ratchet),
      XEdDSA pre-key signatures, WAProto `Message`, 1:1 text send/receive.
      Verified against libsignal and protobufjs.
- [x] **Phase 3b — Direct history**: `HistorySync` notification → CDN download →
      media-key decrypt → inflate → parse → chats/messages in the UI.
- [x] **Phase 4 — Groups + media**: sender-key (`skmsg`) groups + group
      metadata, media upload/download (CDN), verified against Baileys'
      `GroupCipher` and `encryptedStream()`.
- [x] **Phase 5 — Qt 4.7.4 back-port**: JSON shim, RFC 6455 WebSocket on
      `QSslSocket`, functor-connect hook, multipart/CLI/query replacements;
      `qt/tests/qt4_check.sh` compiles every Qt source against real 4.7.4
      headers.
- [ ] **Phase 6 — App state + USync**: mute/pin/archive patches, USync device
      lists, media thumbnails, `QtSql` store, communities.

## 8. Verification

`qt/tests/run_tests.sh` compiles the Qt-free protocol code with the host g++
and checks it against vectors generated from the real Baileys package and
Node's `crypto` (set `BAILEYS_DIR` to run the interop phases as well):

| Suite | What it proves |
|---|---|
| `test_codec` | decode→encode is byte-identical for real Baileys frames; DEFLATE matches Node zlib; compressed (flag `0x02`) frames decode |
| `test_crypto` | SHA-256/HMAC/HKDF, X25519 (incl. RFC 7748), AES-256-GCM (+tampering rejected), AES-256-CBC |
| `test_proto` | `ClientHello`, `ServerHello` decode, `ClientFinish`, `DeviceProps`, registration + login `ClientPayload` identical to protobufjs |
| `test_noise` | the full Noise key schedule produces the same ciphertexts as Baileys' `NoiseHandler`, plus frame header/split/coalesce handling |
| `test_ed25519` | SHA-512 and XEdDSA signatures are byte-identical to curve25519-js, both verify, tampering and bad signatures are rejected |
| `test_signal` | X3DH/Double Ratchet: libsignal decrypts our `PreKeyWhisperMessage` and we decrypt its replies (two-phase Node run) |
| `test_wa_proto` | `Message`/`HistorySync`/`HistorySyncNotification` encode **and** decode match protobufjs byte-for-byte, including padding round-trips |
| `test_history` | a real Baileys-produced `HistorySync` blob decrypts (HKDF + AES-CBC + 10-byte HMAC), inflates through the zlib wrapper and flattens into chats/messages/pushnames |
| `test_message` | full message layer: parse the real `<iq xmlns=encrypt>` bundle node, build a `<message>` stanza, libsignal decrypts it (text + `deviceSentMessage`), and C++ decrypts libsignal's `pkmsg`/`msg` replies into the right chat; also group stanzas (`skmsg` body + distribution fan-out, sender-key memory), group metadata parse and skmsg receive, and the group-administration set (create/leave/subject/description/participants/invite/accept incl. their error and per-participant status parsing, plus a wire round-trip of `<create>`) |
| `test_sender_key` | Baileys' `GroupCipher` decrypts our `skmsg` (HKDF `WhisperGroup`, XEdDSA); we decrypt theirs including its skipped iterations (0, 2, 4…), reject replays/tampering/wrong key id, and re-key cleanly |
| `test_media` | Baileys' `encryptedStream()` ciphertext/hashes for the same file+mediaKey are identical, the CDN path (`base64url(SHA-256(cipher‖MAC))`) and upload URL match, tampered blobs are rejected, and both directions decrypt; ImageMessage/DocumentMessage decode against protobufjs; stickers use the Image key mapping, `/mms/image` and a round-tripping `StickerMessage`; `oggOpusDuration` reads the Ogg granule position |
| `qt4_check.sh` | every Qt source compiles against the real Qt 4.7.4 headers (`QT_VERSION 0x040704`) — no Qt 4.8+/5/6 API can creep back in |
| `gcc46_check.sh` | no C++ construct the Symbian GCGE (GCC 4.6.3) rejects **and** nothing STLport 5.2 lacks (`std::function`, smart pointers, `initializer_list`, `<random>`, `<cstdint>`, `std::move`, `vector::data()`, `emplace_back`); the Qt-free core also compiles as `-std=c++0x` |
| `moc_check.sh` | no `struct`/`enum`/data member inside a `signals:`/`slots:` block (moc 4.7 aborts on those); runs the real `moc` when present |

Eleven suites run with a plain host `g++`; three of them are three-phase
(C++ → Node/libsignal-or-Baileys → C++). The Qt 4 check is separate because it
needs the Qt 4.7.4 headers (fetched automatically):

```bash
./qt/tests/qt4_check.sh
```

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
- RFC 6455 (WebSocket): <https://datatracker.ietf.org/doc/html/rfc6455>
- Qt 4.7.4 source (the header check): <https://github.com/qt/qt/tree/4.7>
- Prior Qt WhatsApp attempts (WebEngine wrappers, **not** direct protocol):
  WhatSie, WhatsThat (archived 2019) — useful UI reference only.
