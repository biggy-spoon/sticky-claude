# sticky-claude firmware

ESP-IDF (C++) firmware for the Seeed Studio reTerminal Sticky (ESP32-S3R8).
Hold the AI/record button, speak, release — the clip gets POSTed to the
`backend` service, which transcribes it and asks Claude (with calendar
tool-use if you've connected one), and the reply renders on the ePaper
screen. The Up/Down buttons page through each configured plugin's own
on-device screen (see `device_nav`, below). Tap a row on the touchscreen to
select it, and tap a checkable row to complete it (e.g. a todo). A short press
of the AI button also toggles the highlighted row.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
v5.3.x. Built, flashed, and run on a real device — the full
record → transcribe → Claude → display loop is confirmed working on
hardware, not just compiled.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

No WiFi/backend config step needed before flashing — the device handles
that itself on first boot (see "First-run setup" below).

### First-run setup

On first boot (or after a factory reset / NVS erase), the device opens an
open WiFi access point named `StickyClaude-Setup`. Connect to it from a
phone or laptop; a setup page should open automatically (the captive
portal trick), or browse to `http://192.168.4.1/` manually. Enter your
real WiFi credentials and your backend's `/converse` URL
(e.g. `http://192.168.1.50:8000/converse`); the device saves this to NVS
and reboots into normal operation.

## Architecture

| Component | Role |
| --- | --- |
| `board` | GPIO/SPI pin map for the buttons, PDM mic, and ePaper panel |
| `button_service` | Polls the AI/Up/Down buttons, debounced; press-start/press-end callbacks |
| `pdm_mic` | I2S PDM RX capture, 16kHz mono 16-bit |
| `recording_service` | PSRAM clip buffer + WAV encoding |
| `settings_service` | NVS-backed WiFi/backend settings, written by the portal |
| `captive_portal` | First-run AP + DNS hijack + HTTP setup form |
| `wifi_service` | STA connect (using `settings_service` values) + SoftAP mode |
| `assistant_service` | POSTs the WAV to the backend, parses `{transcript, reply}` |
| `device_nav` | Up/Down paging and tap handling for plugin device-views (`GET /api/device/views`), confirm/toggle (`POST /api/plugins/{id}/device-action`) |
| `touch_service` | GT911 bring-up, polling, tap detection and mapping to screen coordinates |
| `epaper_display` | SPI transport + SSD1677 command set + 1bpp framebuffer |
| `ui` | LVGL screens rendered via `epaper_display`. Its own task owns LVGL and the panel, so `Show*` calls never block; if several arrive during a refresh, only the newest is drawn |
| `main` | Wires button hold → record → upload → display, Up/Down → `device_nav`, and first-run routing |

## Known gaps

1. **No pre-roll buffering.** Recording starts exactly when the capture
   task notices the button is down, so the first ~20-50ms of speech right
   at press can get clipped. Followup's `recording_service` solves this
   with a continuously-filled ring buffer armed ahead of the long-press
   threshold; worth porting that pattern if clipped first words become
   annoying in practice.
2. **Button timing not finely tuned.** `kMinHoldMs` (400ms) separates a tap
   from a recording; adjust against how the button feels in hand. Also: the
   AI/record button is physically the same pin as the device's power button,
   which has its own hardware-level long-hold behavior independent of firmware
   — holding it for several seconds may trigger a hardware power cycle
   unrelated to recording.
3. **No SD card or sensor support wired up** — the board has them, this
   project doesn't use them (out of scope for the voice-note flow).
4. **Touch bring-up isn't fully explained.** The GT911 only starts properly here
   with a real RST pulse followed by INT held low for 50ms. Other firmware for
   this board works without that INT step, and we don't yet know why.
5. **No partial e-paper refresh.** Every screen change is a full refresh
   (a few seconds). Fine for this project's rate of UI changes; would
   need real work to support faster partial updates if that ever
   mattered here.

## Licensing note

`components/epaper_display` is **GPLv3**, not MIT like the rest of this
repository — its SSD1677 command sequence is ported from the GPLv3
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) project.
See that directory's own `LICENSE` file, and `docs/ARCHITECTURE.md` for
why a from-scratch version didn't work.
