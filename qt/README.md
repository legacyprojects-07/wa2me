# OpenKnot Qt client

A Qt client for OpenKnot with two modes:

| Mode | Talks to | Status |
|---|---|---|
| **Proxy** (default) | your existing OpenKnot backend (`index.js`) over HTTP | complete — full API coverage |
| **Direct** (`--direct`) | `wss://web.whatsapp.com` itself | text 1:1 + groups, media up/download, history sync |

Proxy mode is the drop-in replacement for the J2ME client. Direct mode is the
Baileys-free path: the Qt app speaks WhatsApp's protocol without any server in
between. Both build against **Qt 4.7.4** (Symbian) as well as Qt 5/6 — see
[`../QT_PORT.md`](../QT_PORT.md) for the mapping and the remaining gaps
(app state, presence privacy).

## Build

Desktop (Qt 5.15 / Qt 6):

```bash
cmake -S qt -B build/qt -DCMAKE_BUILD_TYPE=Release
cmake --build build/qt
./build/qt/openknot-qt --url http://192.168.1.10:3000 --key YOUR_API_KEY
```

Qt 4.7.4 or newer: the only modules needed are `QtCore`, `QtGui` (Widgets) and
`QtNetwork`. Anything newer than 4.7.4 is shimmed in `src/qt4compat/`:

| Qt 5 API | Qt 4.7 replacement |
|---|---|
| `QJsonDocument/Object/Array/Value` (5.0) | `qt4compat/json.{h,cpp}` |
| `QWebSocket` (5.3) | `qt4compat/websocket.cpp` (RFC 6455 on `QSslSocket`) |
| functor `connect()` (5.0) | `SignalHook` in `qt4compat/qt4compat.h` |
| `QHttpMultiPart` (4.8) | hand-built multipart body in `api_client.cpp` |
| `QUrlQuery`, `QCommandLineParser` (5.x) | `QUrl::addQueryItem`, argv parsing |
| `QStringLiteral`, `toHtmlEscaped`, `QDateTime::…SecsSinceEpoch` | `qt4compat.h` |

### Symbian / Qt 4.7

Symbian has no CMake in its toolchain, so `qt/openknot.pro` is the build entry
point there (the same sources and defines):

```bash
qmake -spec symbian-gcce qt/openknot.pro && make      # device
qmake qt/openknot.pro && make                         # desktop Qt 4.7
```

```bash
# verify the sources only use Qt 4.7.4 API (fetches the qt/qt 4.7 branch for
# the real headers; no Qt installation needed)
./qt/tests/qt4_check.sh
```

The protocol code sticks to the C++11 subset **GCC 4.6** (the Symbian GCGE)
understands — no member initialisers in declarations, no `override`, no
`std::to_string`/`stoi`, no `<chrono>`; `src/direct/compat.h` fills those gaps.
`./qt/tests/gcc46_check.sh` enforces it. CMake defaults to C++17 on desktop and
honours `-DCMAKE_CXX_STANDARD=11` for device builds.
Direct mode needs TLS from the platform (Qt 4.7's `QSslSocket`); if the phone's
root store is too old for WhatsApp's certificate chain you can opt in to
ignoring certificate errors with `OPENKNOT_INSECURE_TLS=1`.

## Run

```bash
# proxy mode (default)
openknot-qt --url http://127.0.0.1:3000 --key $API_KEY

# direct mode: connects to WhatsApp Web, writes ~/.openknot-qt/creds.json
openknot-qt --direct

# custom credentials location
openknot-qt --direct --creds /media/mmc1/openknot/creds.json
```

Pairing: **Connection ▸ Show QR / pair device**, then WhatsApp ▸ Linked devices.
The dialog shows the raw pairing string; render it as a QR image with any QR
encoder (`qrcodegen`, QZXing) if you want a scannable picture.

## Layout

```
src/
  main.cpp              entry point + CLI
  models.h              Chat / Message / Contact / GroupInfo (mirrors serializers.js)
  api_client.*          proxy mode: every OpenKnot REST endpoint
  mainwindow.*          UI: chat list, message view, send/attach/save-media, QR dialog
  qt4compat/
    json.*              QJson* for Qt 4.7 (parser + serialiser + COW value tree)
    qt4compat.*         QStringLiteral/toHtmlEscaped shims + SignalHook (functor connects)
    websocket.*         RFC 6455 WebSocket client over QSslSocket
  direct/
    whatsapp_direct.*   websocket lifecycle, handshake, stanza routing, QR pairing
    binary_node.*       BinaryNode codec (Baileys WABinary port, byte-exact)
    wa_tokens.h         token dictionaries (generated from Baileys constants.js)
    inflate.*           raw-DEFLATE decoder for compressed frames (no zlib)
    noise.*             Noise_XX_25519_AESGCM_SHA256 transport (Baileys port)
    sha256.*            SHA-256 / HMAC / HKDF
    x25519.*            Curve25519 ECDH
    aes_gcm.*           AES-256-GCM / CBC / CTR
    proto_min.*         handshake protobuf (HandshakeMessage, ClientPayload, …)
    ed25519.*           SHA-512 / Ed25519 / XEdDSA (Signal signatures)
    signal.*            Signal Protocol: X3DH + Double Ratchet (libsignal port)
    wa_proto.*          WAProto subset: Message, HistorySync, notifications
    media_crypto.*      WhatsApp media keys + AES-CBC/HMAC blob crypto
    wa_history.*        HistorySync decrypt → inflate → parse → chats
    sender_key.*        group sender keys (skmsg): chains, XEdDSA, skipped keys
    media_upload.*      CDN: media_conn, upload URL, encrypt/hash, downloads
    wa_message.*        pre-key bundles, <message>/group metadata build/parse
tests/
  run_tests.sh          builds + runs every core suite with host g++
  test_codec.cpp        BinaryNode codec + DEFLATE, vs real Baileys bytes
  test_crypto.cpp       SHA/HMAC/HKDF/X25519/AES-GCM/CBC vs Node crypto
  test_proto.cpp        handshake protobuf encoders vs protobufjs output
  test_noise.cpp        Noise key schedule vs the real Baileys NoiseHandler
  test_ed25519.cpp      SHA-512 / XEdDSA vs curve25519-js
  test_signal.cpp       Signal sessions, interop with libsignal (3 phases)
  test_wa_proto.cpp     Message/HistorySync vs protobufjs, both directions
  test_history.cpp      real encrypted HistorySync blob → chats/messages
  test_message.cpp      <message> stanza build/parse, group send + group admin, vs libsignal
  test_sender_key.cpp   group sender keys vs Baileys' GroupCipher (3 phases)
  test_media.cpp        upload/download crypto+URLs vs Baileys encryptedStream()
  test_qt4compat.cpp    Qt 4-only: JSON shim round-trips (creds.json)
  qt4_check.sh          compiles every Qt source against real Qt 4.7.4 headers
  gcc46_check.sh        rejects anything the Symbian GCC 4.6.3 cannot compile
  verify_signal.mjs     phase 2 of test_signal (decrypts/encrypts via libsignal)
  verify_message.mjs    phase 2 of test_message (decodes the stanza, replies)
  verify_group.mjs      phase 2 of test_sender_key (GroupCipher decrypt/reply)
  verify_media.mjs      phase 2 of test_media (proto decode + CDN decrypt)
  gen_*.mjs             regenerate the vectors from Baileys (`--regen`)
  *_vectors.txt         checked-in vectors (Baileys 6.7.22)
```

## Tests

The protocol code is Qt-free, so it is verified on a desktop toolchain without
installing Qt:

```bash
./qt/tests/run_tests.sh
# or, including the libsignal interop phases:
BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys ./qt/tests/run_tests.sh
```

```
=== BinaryNode codec ===   decode+re-encode byte parity : 9 passed
=== raw DEFLATE ===        inflateRaw vs Node zlib      : 6 passed
=== compressed frames ===  flag-0x02 frame decode       : 9 passed
=== crypto ===             55 checks, 0 failed
=== handshake protobuf === 10 checks, 0 failed
=== Noise transport ===    15 checks, 0 failed
=== Ed25519 / XEdDSA ===   31 checks, 0 failed
=== WAProto ===            58 checks, 0 failed
=== history / media ===    59 checks, 0 failed
=== Signal (vs libsignal) ===       16 + 18 checks, 0 failed
=== message layer (vs libsignal) === 107 + 18 checks, 0 failed
=== group sender keys (vs Baileys GroupCipher) === 42 + 10 checks, 0 failed
=== media upload/download (vs Baileys) === 89 + 12 checks, 0 failed
```

Eleven suites; `run_tests.sh` builds and runs them all with a plain host g++
(no Qt needed).

The Signal and message suites are three-phase: the C++ side encrypts, a Node
script driving the real libsignal decrypts (and replies), then C++ decrypts the
reply. Set `BAILEYS_DIR` to include them.

Every vector is produced by the actual `@whiskeysockets/baileys@6.7.22` code or
Node's `crypto`, not copied by hand. Regenerate with:

```bash
BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys \
  ./qt/tests/run_tests.sh --regen
```

## What works, what doesn't (direct mode)

Working: websocket connect (`wss://web.whatsapp.com/ws/chat`, browser Origin +
UA), Noise XX handshake, frame framing and compressed-frame decoding, QR
pairing, `<success>`/`<failure>`/`<stream:error>`, keepalive pings,
presence/chatstate/receipt signals, credential persistence, 5-second reconnect
— **plus, with no external crypto libraries at all**:

- **Sending text** (1:1): one-time pre-key upload, `<iq xmlns=encrypt>` device
  bundle fetch (cached 5 min), Signal encryption per device (`pkmsg` first,
  `msg` afterwards), `deviceSentMessage` copies for our own devices, receipts.
- **Receiving text** (1:1): `<enc>` decryption, PKCS#7-ish unpadding, `Message`
  decoding, `deviceSentMessage` unwrapping, chat-routing, read/delivery
  receipts.
- **History sync**: `HistorySyncNotification` → CDN download → media-key
  decrypt (HKDF + AES-256-CBC + HMAC) → inflate → `HistorySync` parse →
  chats, messages and pushnames in the chat list.
- **Group messages**: `<iq xmlns=w:g2><query request=interactive/></iq>`
  metadata, one sender-key chain per group (`skmsg` body with XEdDSA
  signature), sender-key distribution messages fanned out to devices that do
  not have them yet (sender-key memory), skipped-key handling on receive,
  chains persisted in `creds.json`.
- **Group administration**: create, join by invite code/link, leave, add /
  remove / promote / demote participants, subject, description, and invite
  code get/revoke — the same `w:g2` stanzas Baileys sends, in both modes
  (`Group` menu, plus a "Group info" view with admin roles). Incoming `<notification type="w:gp2">` updates rename the
  chat and drop stale metadata; `<notification type="encrypt">` triggers a
  pre-key top-up.
- **Media**: attach a file → fresh 32-byte mediaKey → `HKDF("WhatsApp <Type>
  Keys")` → AES-256-CBC + 10-byte HMAC → `<iq xmlns=w:m><media_conn/></iq>` →
  POST to the CDN → message with `url/directPath/mediaKey/fileSha256/
  fileEncSha256/fileLength`. Every template is covered: image, video,
  audio, document, **sticker** (`.webp` → `StickerMessage` on the same
  `/mms/image` upload path with the Image key mapping) and **voice notes**
  (`File ▸ Send voice note…`, `ptt=true` + duration read from the Ogg granule
  position). Downloads go the other way (`File ▸ Save media from chat…`),
  MAC-verified before writing.

Missing: app state (mute/pin/archive), communities, presence privacy, USync
device lists (bundles are fetched per send), media thumbnails. See
`QT_PORT.md` §3 and §6.
