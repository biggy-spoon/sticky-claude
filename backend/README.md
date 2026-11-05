# sticky-claude backend

Small FastAPI service meant to run on an always-on home server (Pi or ZimaOS).
It is the proxy the reTerminal Sticky firmware talks to: the device uploads a
WAV recording, this service transcribes it locally with
[faster-whisper](https://github.com/SYSTRAN/faster-whisper), sends the
transcript to the Claude API, and returns `{ "transcript": ..., "reply": ... }`
for the device to display.

## Run locally

```bash
cd backend
cp .env.example .env   # fill in ANTHROPIC_API_KEY
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000
```

First request will download the `small` Whisper model (~500MB); switch
`WHISPER_MODEL_SIZE` to `tiny` or `base` in `.env` if the Pi is slow or short
on RAM.

## Run with Docker

```bash
docker build -t sticky-claude-backend .
docker run -p 8000:8000 --env-file .env sticky-claude-backend
```

## Dashboard

Open `http://<backend-host>:8000/dashboard` in a browser. It shows recent
conversations, device status, each installed plugin's settings (and, for
plugins that have one, a data view — calendar's is "Upcoming events"),
and lets you edit the Anthropic API key and Claude model. None of this
needs touching `.env` or restarting (the Whisper settings are the
exception — those load once at startup, so changing them still needs a
restart).

**There is no authentication on the dashboard or its API.** It's meant for
your home LAN, same trust model as the device's own setup portal. Don't
port-forward it to the public internet without putting something in front
of it.

## Plugins

Claude's capabilities beyond plain conversation — calendar access, a
todo list — come from plugins (`plugins/`). A plugin declares the Claude
tools it contributes, the settings fields it needs (rendered generically
by the dashboard — a plugin with only text/password fields needs zero
frontend changes), and optionally extra HTTP routes for its own setup
flow (calendar's Google OAuth callback, for example).

Every `.py` file in `plugins/` is scanned at import time for a
`plugins.base.Plugin` subclass and registered automatically — that's the
whole install story, for a built-in or a third-party plugin alike. **To
install a community plugin**, copy its file(s) into `plugins/` and
restart the backend; nothing else to edit. The dashboard's "Plugin hub"
card lists what's available from
[sticky-claude-plugin-hub](https://github.com/biggy-spoon/sticky-claude-plugin-hub)
(a metadata index only — the backend never fetches or runs a plugin's
code on its own, so review a plugin before dropping it in).

See `plugins/base.py` for the contract, and `plugins/calendar_plugin.py`
/ `plugins/todo_plugin.py` for the two reference implementations (one
with external-service settings, one with none). Want to publish your
own? See that repo's `CONTRIBUTING.md`.

### Calendar plugin setup

Pick one in the dashboard's Calendar card:

**CalDAV** (iCloud, Fastmail, Nextcloud, or any other CalDAV server):
1. For iCloud specifically: go to [appleid.apple.com](https://appleid.apple.com) →
   Sign-In and Security → App-Specific Passwords, and generate one. Don't
   use your real Apple ID password.
2. In the dashboard, set CalDAV URL to `https://caldav.icloud.com` (or your
   provider's CalDAV endpoint), Username to your account email, and
   Password to the app-specific password.
3. Leave "Calendar name" blank to use the account's first calendar, or
   enter one to target a specific calendar.

**Google Calendar**:
1. In the [Google Cloud Console](https://console.cloud.google.com/), create
   a project (or use an existing one), enable the Calendar API, and create
   an OAuth 2.0 Client ID (type: Web application). Add
   `http://<backend-host>:8000/api/plugins/calendar/google/callback` as
   an authorized redirect URI.
2. In the dashboard, enter the client ID and secret, save, then click
   "Connect with Google" and sign in. That exchanges a one-time code for a
   refresh token, which gets stored so the backend can call the Calendar
   API without you signing in again.

Once either is configured, Claude gets `create_calendar_event` and
`list_calendar_events` tools automatically — ask the device to add or
check something on your calendar and it'll actually do it, not just talk
about it.

## API

- `GET /health` — model/config status.
- `POST /converse` — body is raw WAV bytes (16-bit PCM mono, see firmware's
  `recording_service`). Returns JSON `{"transcript": "...", "reply": "..."}`.
- `GET /dashboard` — the web dashboard.
- `GET /api/history`, `GET /api/status`, `GET`/`POST /api/settings` —
  core data, also usable directly.
- `GET /api/plugins` — every plugin's id/name/description/configured
  state and settings schema (what the dashboard renders its forms from).
- `POST /api/plugins/{id}/settings` — save a plugin's settings.
- `GET /api/plugins/{id}/view` — a plugin's dashboard data, if it has one
  (calendar returns `{"events": [...]}`, todo returns
  `{"open": [...], "done": [...]}`).
- `GET /api/plugin-hub` — the community plugin index (name, description,
  repo URL, install state), proxied and cached from
  `sticky-claude-plugin-hub`.
- Plugins may add their own routes too — calendar adds
  `GET /api/plugins/calendar/google/login` and `/callback` for its OAuth
  flow.

## Sizing note

`faster-whisper` on CPU is the main latency cost. On a Raspberry Pi 4/5,
`small` + `int8` is usually a few seconds for a 5-10s clip; ZimaOS hardware
with more cores will be faster. Tune `WHISPER_MODEL_SIZE` for your box.
