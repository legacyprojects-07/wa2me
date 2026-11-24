# OpenKnot
### Deploy on Render (free)

1. Push to GitHub
2. [render.com](https://render.com) → **New** → **Blueprint** → connect repo
3. Render auto-creates a PostgreSQL database + web service
4. Open `https://your-app.onrender.com/qr-image` → scan QR
#### Based on baileys, Baileys is a library which connects directly to WA WEB SOCKETS. NO 3rd party service involved while connecting to WhatsApp.

---

## Qt client (`qt/`) — direct WhatsApp Web, no server in between

`qt/` contains a Qt port for the Symbian/Qt app, so it can talk to WhatsApp
itself instead of going through this backend.

* **Proxy mode (default, complete)** — Qt replacement for the J2ME client: every
  REST endpoint in `index.js` (`/chats`, `/messages`, `/send`, `/send-media`,
  `/sync`, groups, presence, search, …) via `QNetworkAccessManager`. Needs no
  crypto libraries, so it runs on Symbian Qt 4.7.
* **Direct mode (`--direct`)** — the app opens `wss://web.whatsapp.com/ws/chat`
  itself: Noise handshake, BinaryNode codec, QR pairing, keepalive, reconnect,
  **Signal-encrypted 1:1 text send/receive** and **chat history sync**. The
  whole stack — Noise, X25519, AES-GCM/CBC, HKDF, DEFLATE, XEdDSA, the Signal
  double ratchet, the WAProto subset and WhatsApp media crypto — is hand-written
  with **no libsodium, zlib, protobuf or libsignal**, and every layer is
  verified against `@whiskeysockets/baileys@6.7.22` (including real libsignal
  and protobufjs interop in `qt/tests/`).

```bash
cmake -S qt -B build/qt && cmake --build build/qt
./build/qt/openknot-qt --url http://127.0.0.1:3000 --key $API_KEY   # proxy
./build/qt/openknot-qt --direct                                    # direct
./qt/tests/run_tests.sh    # protocol test suites, no Qt needed
BAILEYS_DIR=/path/to/@whiskeysockets/baileys ./qt/tests/run_tests.sh  # + libsignal interop
```

Read [`QT_PORT.md`](QT_PORT.md) for the architecture, the Baileys→C++ mapping,
and exactly what direct mode still needs (group sender keys, media upload and
app state).
See [`qt/README.md`](qt/README.md) for build/run details.
##### This project is review coded, I have tried my best to improve it still there might be some bugs/quirks. 
###### Enjoy!
