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
tests/
  run_tests.sh          builds + runs all four core suites with host g++
  test_codec.cpp        BinaryNode codec + DEFLATE, vs real Baileys bytes
  test_crypto.cpp       SHA/HMAC/HKDF/X25519/AES-GCM/CBC vs Node crypto
  test_proto.cpp        handshake protobuf encoders vs protobufjs output
  test_noise.cpp        Noise key schedule vs the real Baileys NoiseHandler
  gen_*.mjs             regenerate the vectors from Baileys (`--regen`)
  *_vectors.txt         checked-in vectors (Baileys 6.7.22)
```

## Tests

The protocol code is Qt-free, so it is verified on a desktop toolchain without
installing Qt:

```bash
./qt/tests/run_tests.sh
```

```
=== BinaryNode codec ===   decode+re-encode byte parity : 9 passed
=== raw DEFLATE ===        inflateRaw vs Node zlib      : 6 passed
=== compressed frames ===  flag-0x02 frame decode       : 9 passed
=== crypto ===             55 checks, 0 failed
=== protobuf ===           10 checks, 0 failed
=== Noise transport ===    15 checks, 0 failed
```

Every vector is produced by the actual `@whiskeysockets/baileys@6.7.22` code or
Node's `crypto`, not copied by hand. Regenerate with:

```bash
BAILEYS_DIR=/path/to/node_modules/@whiskeysockets/baileys \
  ./qt/tests/run_tests.sh --regen
```

## What works, what doesn't (direct mode)

Working: websocket connect (`wss://web.whatsapp.com/ws/chat`, browser Origin +
UA), Noise XX handshake with the real key schedule, frame framing and
compressed-frame decoding, QR pairing stanza handling, `<success>`/`<failure>`/
`<stream:error>` handling, keepalive pings, presence/chatstate/receipt signals,
credential persistence and 5-second reconnect.

Missing (and why): Signal (X3DH + Double Ratchet) message encryption needs
libsignal-protocol-c, and `Message`/`HistorySync` decoding needs the generated
WAProto. Until both are in, direct mode pairs and stays connected but cannot
read or send message bodies — every call site is marked `TODO(signal)` /
`TODO(proto)` and listed in `QT_PORT.md` §6.
