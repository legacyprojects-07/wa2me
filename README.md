# WhatsApp Nokia Backend (Baileys + Render, no database)

Milestone 1: connect to WhatsApp via QR, keep the session alive on Render's
free tier by pinging it with an uptime bot (so the process/disk never idles
out), and expose a couple of plain HTTP/JSON endpoints a J2ME MIDlet can call.

## How session persistence works here

Baileys writes its auth session to local files (`useMultiFileAuthState`,
default folder `./auth_state`). There's no database — the trade-off is:

- **Survives**: idle spin-down, as long as an uptime bot (e.g. UptimeRobot,
  cron-job.org) pings the service often enough that Render never puts the
  container to sleep. The process keeps running, disk stays intact, no
  re-scan needed.
- **Does NOT survive**: a redeploy (new commit pushed), a manual restart, or
  a crash. Render free-tier containers get a fresh filesystem on any of
  those — `auth_state/` is gone and you'll need to scan the QR again.

If that's an acceptable trade-off for a hobby project, this is the simplest
setup. If you later want sessions to survive redeploys too, that's what
external storage (Mongo, R2, etc.) would buy you — happy to add it back in
if it becomes annoying.

## 1. Set up the uptime bot

Pick one (both have free tiers):
- **UptimeRobot** — https://uptimerobot.com
- **cron-job.org** — https://cron-job.org

Point it at `GET https://<your-render-url>/health` on an interval shorter
than Render's free-tier idle timeout (currently 15 minutes — ping every
5–10 minutes to be safe).

## 2. Local test (optional, before deploying)

```bash
npm install
cp .env.example .env
npm start
```

Then:
```bash
curl http://localhost:3000/qr
```
This returns a base64 PNG data URL. Render it (e.g. paste into a "data URL
to image" tool) and scan with WhatsApp → Linked Devices → Link a Device.

Once scanned, `GET /health` should report `"status": "connected"`, and
`GET /chats` should return your chat list.

## 3. Deploy to Render (free tier)

1. Push this folder to a GitHub repo (note: `.gitignore` excludes
   `auth_state/`, since it shouldn't live in git — it's per-deployment).
2. On Render: New → Web Service → connect the repo.
3. Build command: `npm install`
4. Start command: `npm start`
5. No env vars required (PORT is set automatically by Render).
6. Deploy, then immediately hit `/qr` and scan before the uptime bot's
   first ping cycle — you want to catch the QR before the socket cycles.
7. Add the deployed URL to your uptime bot (step 1) right after confirming
   `/health` says `connected`.

## 4. Endpoints (current milestone)

| Method | Path              | Purpose                                      |
|--------|-------------------|-----------------------------------------------|
| GET    | `/health`         | `{ status: "starting" \| "qr" \| "connected" \| "disconnected" }` |
| GET    | `/qr`             | Base64 PNG data URL of the current QR code   |
| GET    | `/chats`          | List of known chats, sorted by recency       |
| GET    | `/messages/:jid`  | Last ~50 messages for a chat (URL-encode the JID) |
| POST   | `/send`           | `{ "jid": "...", "text": "..." }`            |

## Notes / things to watch for

- **Chat list is in-memory.** It rebuilds from Baileys' `messaging-history.set`
  event on each fresh connection, so right after any reconnect, `/chats` may
  be empty for a few seconds until history sync completes.
- **Every redeploy = re-scan.** Since there's no external store, pushing new
  code means Render provisions a fresh container with no `auth_state/`.
  Budget for scanning the QR again each time you ship a backend change.
- **Baileys version drift**: this library's internal events/APIs change
  fairly often between minor versions. If `messaging-history.set` or
  `chats.upsert` don't fire as expected, check the installed version's
  CHANGELOG on GitHub — the shape of update objects has moved before.
- Next milestones to build after this: pagination for `/messages`, media
  download endpoints (images/audio), and the actual J2ME client polling
  loop + UI.
