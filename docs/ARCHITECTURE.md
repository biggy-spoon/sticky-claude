# Architecture

## Hardware: reTerminal Sticky

- SoC: ESP32-S3R8 (dual-core, up to 240MHz, WiFi 4 + BLE 5.0), 8MB PSRAM, 32MB flash
- Display: 3.97" monochrome ePaper, 800x480, SSD1677 controller, SPI
- Audio: single PDM microphone (no speaker — this device is **text-out only**)
- AI/record button is the long-press trigger (push-to-talk). It's
  physically the same pin as the device's power button, which has its
  own hardware-level long-hold behavior independent of firmware — keep
  holds in the roughly 0.3-3 second range; much longer holds can trigger
  a hardware power cycle unrelated to recording.

Pin map lives in `firmware/components/board/include/board_config.h`,
confirmed against the
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) open-source
firmware for the same board (the pin assignments are a fact about how the
board is wired, not anyone's creative work, so reusing them doesn't carry
license obligations — only porting actual driver/business logic would,
which is exactly what happened for the ePaper driver; see "Licensing"
below).

## Why a backend proxy instead of calling Claude directly from the device

The ESP32-S3 can't run STT or an LLM itself. The device talks only to a
backend service on the user's own always-on Pi or ZimaOS box; that service
does local speech-to-text (faster-whisper), the Claude API call (with
calendar tool-use), and returns plain text for the device to show. This
keeps the Anthropic API key off the device entirely and keeps the
device's only network dependency a LAN address instead of a public cloud
endpoint for STT.

## First-run setup

The device ships unconfigured. On boot, `settings_service` checks NVS for
saved WiFi credentials and a backend URL; if none exist, `main.cpp` starts
`captive_portal` instead of the normal flow: an open WiFi AP
(`StickyClaude-Setup`), a DNS server that answers every query with
its own IP (the standard trick that makes phones auto-open the setup
page), and an HTTP form. Submitting the form saves to NVS and reboots the
device into normal operation. This replaced an earlier Kconfig/recompile-
based setup entirely.

## End-to-end flow

1. User holds the AI button → `button_service` fires press-start →
   `recording_service` arms the PDM mic and starts filling a PSRAM buffer
   → `ui` shows the "recording" screen.
2. User releases → `recording_service` stops, builds a 16kHz mono PCM16
   WAV in memory. `ui` shows the "thinking" screen.
3. `assistant_service` POSTs the WAV to the backend's `/converse`
   endpoint (URL from `settings_service`).
4. Backend (`backend/app.py`) transcribes with faster-whisper, then sends
   the transcript to Claude. If a calendar is connected,
   `create_calendar_event`/`list_calendar_events` tools are offered, and
   a bounded tool-use loop (`calendar_service`) executes them against
   CalDAV or Google Calendar before looping back to Claude for the final
   reply. The backend logs the exchange to SQLite (`storage.py`) and
   returns `{"transcript": ..., "reply": ...}`.
5. Device receives the reply and `ui` renders it on the ePaper panel via
   LVGL (idle/recording/thinking/reply screens, built against
   `epaper_display`'s framebuffer).

## On-device UI (LVGL)

`components/ui` integrates LVGL (pulled as a managed component) against
`epaper_display`'s 1bpp framebuffer. Rather than using LVGL's native
`LV_COLOR_DEPTH_1` packed buffer format (whose exact bit layout wasn't
worth gambling on without hardware to iterate against), the display
driver uses the normal color-depth renderer and a custom `flush_cb` that
thresholds each pixel's brightness into `epaper_display::SetPixel`.
`full_refresh` is forced on, so every screen change is a genuine full
e-paper refresh (a few seconds) — there's no partial-refresh support, and
screens only update on explicit state changes, never animated.

## The ePaper driver, and why it's GPLv3

The SSD1677 init/refresh command sequence in
`firmware/components/epaper_display` is ported from the `folloup-sticky`
project's `ssd1677_driver.cpp` (GPLv3), after a from-scratch version
(written from generic SSD16xx recollection, not a datasheet) compiled and
ran without error but never produced a visible update once real content
replaced an early solid-block test. The proven sequence writes to *both*
the SSD1677's "new" and "old" RAM planes (this controller computes its
waveform from the difference between them), includes a booster
soft-start sequence and a Display Update Control 1 command the
from-scratch version omitted entirely, and addresses its RAM X window in
pixels rather than bytes. Because it's a port of GPLv3 code, that one
directory is GPLv3 (see its own `LICENSE` file) while the rest of this
repository is MIT.

## Backend: dashboard, plugins, and storage

- `storage.py` — SQLite: conversation history, device last-seen/IP, and
  a flat settings key-value table.
- `settings.py` — layers the settings table over `.env` as a first-run
  fallback, so the Anthropic key and Claude model are hot-editable from
  the dashboard without a restart. Whisper settings stay env-only since
  the model loads once at startup.
- `plugins/` — how Claude gets capabilities beyond plain conversation.
  `base.py` defines the contract: a plugin declares the Claude tools it
  contributes (`get_tools()`/`execute_tool()`), a declarative settings
  schema the dashboard renders generically (`settings_schema()`,
  including conditional fields and action buttons), and optionally its
  own HTTP routes (`register_routes()`) for things like an OAuth
  callback. `app.py`'s `/converse` handler collects tools from every
  *configured* plugin, offers them all to Claude in one request, and
  routes any `tool_use` block back to whichever plugin owns that tool
  name — `app.py` itself has no plugin-specific logic.
- `calendar_plugin.py` — the first (and so far only) plugin, implementing
  `create_calendar_event`/`list_calendar_events` on top of
  `calendar_service.py`'s provider-agnostic `CalendarProvider` interface:
  `CalDavProvider` (iCloud/Fastmail/Nextcloud/etc, via the `caldav`
  package building ICS with `icalendar`) and `GoogleCalendarProvider`
  (OAuth, via `google-api-python-client`). `calendar_service.py` stays
  pure domain logic with no knowledge of the plugin contract; the plugin
  is a thin adapter over it.
- `dashboard.html` + the `/api/*` routes in `app.py` — a single
  dependency-free HTML/JS page served at `/dashboard`. Settings forms are
  built client-side from each plugin's schema; a plugin-specific data
  view (like calendar's "Upcoming events") is the one place the
  dashboard still has to know a plugin by name rather than purely by
  schema. No authentication — same trust model as the device's own setup
  portal, meant for the home LAN only.

## Open items

See `firmware/README.md` → "Known gaps" for the concrete list (no
pre-roll audio buffering, button hold-timing not finely tuned, no touch/
SD/sensor support wired up — the board has them, this project doesn't
use them).
