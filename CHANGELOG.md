# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
This project doesn't have tagged releases yet -- everything below is
tracked under Unreleased until there's a first stable cut.

## [Unreleased]

### Added
- Project scaffold: ESP-IDF firmware for the reTerminal Sticky (button,
  PDM mic, WiFi, ePaper SPI driver, HTTP client) and a FastAPI backend
  (faster-whisper transcription + Claude API call).
- Browser-based first-run setup: the device opens an AP + captive portal
  for entering WiFi credentials and the backend URL, replacing
  Kconfig/recompile-based configuration. Settings persist in NVS.
- LVGL-based on-device UI: idle, recording, thinking, and reply screens
  with real typography, replacing a placeholder status bar.
- Backend dashboard (`/dashboard`): conversation history, device status,
  upcoming calendar events, and editable settings (API key, Claude model,
  calendar connection) with no restart required for those.
- Calendar integration: Claude gets `create_calendar_event` and
  `list_calendar_events` tools once a calendar is connected, via either
  CalDAV (iCloud, Fastmail, Nextcloud, etc.) or Google Calendar (OAuth).
  Backed by a new SQLite store for conversation history, device status,
  and mutable settings.
- Plugin system (`backend/plugins/`): generalized the calendar
  integration into the first implementation of a `Plugin` contract
  (tool definitions, a declarative settings schema the dashboard renders
  generically, optional custom routes). `app.py`'s `/converse` handler
  and the dashboard's settings UI are now plugin-generic -- neither has
  calendar-specific logic left. A new plugin needs no backend or
  dashboard changes beyond implementing the contract and registering it.

### Fixed
- `epaper_display`'s SSD1677 command sequence, written from generic
  SSD16xx recollection at scaffold time, never produced a visible update
  once real (non-solid-block) content needed rendering. Ported the
  proven sequence from the `folloup-sticky` reference project (same
  board/panel) -- it was missing the "old" RAM plane write, the booster
  soft-start sequence, and Display Update Control 1 entirely, and used
  byte-address RAM-X-window granularity where this controller expects
  pixel addressing. Confirmed visually on hardware afterward.
  `firmware/components/epaper_display` is GPLv3 as a result (ported from
  a GPLv3 source); the rest of the repository stays MIT.
- SPI DMA transfer-size overflow: a single `spi_device_transmit` call
  covering the full ~48KB framebuffer exceeded the ESP32-S3's
  `SPI_LL_DMA_MAX_BIT_LEN` (32768 bytes); chunked into 4096-byte writes.
- Two real ESP-IDF compile errors caught by actually building against the
  toolchain for the first time: a nonexistent I2S PDM slot-config macro
  name, and a component missing `esp_timer` in its `CMakeLists.txt`
  `REQUIRES` (only fails once something actually includes the header).
- Backend: `faster-whisper==1.0.3` with an unconstrained `av` dependency
  resolved to a combination that crashed every real transcription
  (`av.open()` no longer accepts the kwarg `faster-whisper`'s
  `decode_audio()` passes). Bumped `faster-whisper` and pinned `av` to a
  combination confirmed to work.
- Backend: Claude API exceptions in the `/converse` handler weren't
  caught, so a real API failure (bad key, rate limit, network error)
  surfaced to the device as an opaque 500 instead of a useful error.
- Backend: `anthropic==0.40.0` throws `AttributeError` parsing any
  response containing `tool_use` blocks on this stack (its
  discriminated-union handling). Upgraded to `2.5.0`.
- The calendar plugin's dashboard view (`GET /api/plugins/calendar/view`)
  didn't catch provider exceptions, so a misconfigured or temporarily
  unreachable calendar crashed the whole endpoint with a 500 instead of
  showing a clean error in the dashboard.

## Earlier

Everything before the dashboard/calendar work was built and verified in
one continuous session: scaffold, hardware bring-up (ESP-IDF toolchain
install, first real build, first flash), the captive portal, the LVGL
UI, and the display driver fix, each confirmed against real hardware
(a Seeed Studio reTerminal Sticky) before moving on. See the git history
for the detailed story -- several of these commits document a real bug
found by actually running the thing, not just compiling it.
