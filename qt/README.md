# OpenKnot Qt client

A Qt client for OpenKnot with two modes:

| Mode | Talks to | Status |
|---|---|---|
| **Proxy** (default) | your existing OpenKnot backend (`index.js`) over HTTP | complete — full API coverage |
| **Direct** (`--direct`) | `wss://web.whatsapp.com` itself | transport + pairing done; message encryption pending |

Proxy mode is the drop-in replacement for the J2ME client and runs on Symbian
Qt 4.7 (HTTP only, no crypto). Direct mode is the Baileys-free path: the Qt app
speaks WhatsApp's protocol without any server in between. See
[`../QT_PORT.md`](../QT_PORT.md) for what direct mode still needs.

## Build

Desktop (Qt 5.15 / Qt 6):

```bash
cmake -S qt -B build/qt -DCMAKE_BUILD_TYPE=Release
cmake --build build/qt
./build/qt/openknot-qt --url http://192.168.1.10:3000 --key YOUR_API_KEY
```

Qt 5.4+ is required for direct mode (`QWebSocket`). Proxy mode needs only
`QtCore` + `QtNetwork`.

### Symbian / Qt 4.7

Proxy mode only:

* replace `QJsonDocument` with a JSON shim (`api_client.cpp` is the only user),
* drop `Qt::Sql`/`WebSockets` from `CMakeLists.txt`,
* build with the Symbian Qt 4.7 toolchain (`qmake` + a `.pro` file is enough —
  the sources have no Qt5-only APIs other than JSON).

Direct mode on Symbian additionally needs a `QTcpSocket`-based WebSocket client
(Qt < 5.4 has none) — see `QT_PORT.md` §5.

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
  mainwindow.*          UI: chat list, message view, send, QR dialog, log dock
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
    wa_message.*        pre-key bundles, <message> build/parse, deviceSent
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
  test_message.cpp      <message> stanza build/parse, interop with libsignal
  verify_signal.mjs     phase 2 of test_signal (decrypts/encrypts via libsignal)
  verify_message.mjs    phase 2 of test_message (decodes the stanza, replies)
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
=== message layer (vs libsignal) === 29 + 18 checks, 0 failed
```

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

Missing: groups (`skmsg` sender keys + group metadata), media upload/download
in the UI, app state (mute/pin/archive), USync device lists (bundles are
fetched per send). See `QT_PORT.md` §3 and §6.
