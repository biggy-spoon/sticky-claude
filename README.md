# sticky-claude

[![CI](https://github.com/biggy-spoon/sticky-claude/actions/workflows/ci.yml/badge.svg)](https://github.com/biggy-spoon/sticky-claude/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Firmware license: GPLv3](https://img.shields.io/badge/firmware%2Fepaper_display-GPLv3-orange.svg)](firmware/components/epaper_display/LICENSE)

A push-to-talk voice assistant on a [Seeed Studio reTerminal Sticky](https://www.seeedstudio.com/sticky/),
proxied through your own always-on server to Claude. No cloud middleman
you don't control, no subscription beyond what you already pay for
Claude API usage (which, for occasional voice notes, runs to cents a
month, not dollars).

Hold the button, speak, release. The reply shows up on the device's
e-paper screen a few seconds later. Ask it to put something on your
calendar and it actually will.

## Features

- **Push-to-talk voice notes**, answered by Claude, shown on the
  device's e-paper display with real typography (not a status bar).
- **On-device plugin views**: the Up/Down buttons page through each
  configured plugin's own screen (upcoming calendar events, your todo
  list) without touching the backend dashboard; a short tap of the AI
  button confirms/toggles whatever's highlighted (checking off a todo,
  for instance).
- **No-recompile setup**: first boot opens a WiFi access point with a
  browser-based setup page for WiFi credentials and your backend's
  address. Settings are saved to the device's flash.
- **A web dashboard** on the backend: conversation history, device
  status, editable core settings (API key, Claude model), and every
  plugin's own settings and data view (calendar's upcoming events, the
  todo list's open/done items) — no `.env` editing or restart needed for
  those.
- **A plugin system** for giving Claude new capabilities, shipping with
  two: calendar access (CalDAV or Google Calendar — "add a dentist
  appointment at 3pm on the 24th" actually lands on your calendar) and a
  todo list ("add buy milk to my todo list"). Every plugin is just a
  `.py` file in `backend/plugins/` discovered automatically at startup —
  writing a new one doesn't require touching the core backend, and
  installing someone else's is a copy-and-restart. See
  `backend/plugins/` and the companion
  [`sticky-claude-plugin-hub`](https://github.com/biggy-spoon/sticky-claude-plugin-hub)
  repo, a community index of plugins beyond these two defaults.
- **Runs on hardware you already have**: the backend is a small FastAPI
  service meant for a Raspberry Pi or a ZimaOS box you already leave on.

## How it works

```
reTerminal Sticky  <--WAV / text-->  backend (Pi or ZimaOS)  <--transcript / reply-->  Claude API
  (ESP32-S3 + ePaper)                (whisper STT + tool-use)                                │
                                             │                                                │
                                   create / list event  <─────────────────────────────────────┘
                                             │
                                             v
                                  your calendar (CalDAV or Google)
```

1. Hold the AI button on the device, speak, release.
2. The device records the clip and uploads it over WiFi to your backend.
3. The backend transcribes it locally (faster-whisper, no cloud STT),
   then sends the transcript to Claude — with calendar tools available
   if you've connected one.
4. If Claude needs to create or check a calendar event, the backend
   executes that for real and loops the result back to Claude for a
   natural-language reply.
5. The reply comes back to the device and renders on the e-paper screen.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the hardware
details and design notes behind this.

## Quickstart

**1. Backend** (on your Pi, ZimaOS box, or any always-on machine on your
home network):

```bash
cd backend
cp .env.example .env   # fill in ANTHROPIC_API_KEY
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000
```

Open `http://<that-machine>:8000/dashboard` to connect a calendar and
check status.

**2. Firmware** (requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html) v5.3.x):

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

**3. First boot**: the device opens a WiFi network named
`StickyClaude-Setup`. Connect to it from your phone, a setup page should
open automatically (or browse to `http://192.168.4.1/`); enter your real
WiFi credentials and the backend's address
(`http://<that-machine>:8000/converse`). The device reboots and connects.

See `firmware/README.md` and `backend/README.md` for the full detail,
including known gaps and calendar provider setup.

## Project structure

- `firmware/` — ESP-IDF (C++) firmware: button, mic, WiFi, captive
  portal, LVGL UI, ePaper driver.
- `backend/` — FastAPI service: transcription, Claude calls, a plugin
  system (`backend/plugins/`; calendar and the todo list ship by
  default), and a SQLite-backed dashboard/history/settings.
- `docs/ARCHITECTURE.md` — hardware details and design notes.

## Status

Built and verified end-to-end against real hardware: the full
record → transcribe → Claude (with calendar tool-use) → display loop
works on an actual device. See [`CHANGELOG.md`](CHANGELOG.md) for what's
landed and what's been fixed along the way (several real bugs only
surfaced by actually running this on hardware, not just compiling it).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). This project follows the
[Contributor Covenant](CODE_OF_CONDUCT.md).

## License

MIT (see [`LICENSE`](LICENSE)), **except**
`firmware/components/epaper_display/`, which is GPLv3 — its SSD1677
command sequence is ported from the GPLv3-licensed
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) project
(confirmed working on the same board/panel). See that directory's own
`LICENSE` file.

## Acknowledgments

- [`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) by
  alxv2016 — the reference this project's board pin map and ePaper
  driver are built from/ported from.
- [Seeed Studio](https://www.seeedstudio.com/) for the reTerminal Sticky
  hardware.
