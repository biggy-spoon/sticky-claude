# sticky-claude firmware

ESP-IDF (C++) firmware for the Seeed Studio reTerminal Sticky (ESP32-S3R8).
Hold the AI/record button, speak, release — the clip gets POSTed to the
`backend` service, which transcribes it and asks Claude (with calendar
tool-use if you've connected one), and the reply renders on the ePaper
screen. The Up/Down buttons page through each configured plugin's own
on-device screen (see `device_nav`, below); a short tap of the AI button
confirms/toggles whatever's highlighted there (e.g. checking off a todo).

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
| `device_nav` | Up/Down paging across plugin device-views (`GET /api/device/views`), confirm/toggle (`POST /api/plugins/{id}/device-action`) |
| `epaper_display` | SPI transport + SSD1677 command set + 1bpp framebuffer |
| `ui` | LVGL screens (idle/recording/thinking/reply/loading/plugin-view) rendered via `epaper_display`; idle also shows a page-position dot indicator |
| `main` | Wires button hold → record → upload → display, Up/Down → `device_nav`, and first-run routing |

## Known gaps

1. **No pre-roll buffering.** Recording starts exactly when the capture
   task notices the button is down, so the first ~20-50ms of speech right
   at press can get clipped. Followup's `recording_service` solves this
   with a continuously-filled ring buffer armed ahead of the long-press
   threshold; worth porting that pattern if clipped first words become
   annoying in practice.
2. **Button timing not finely tuned.** `kMinHoldMs` (250ms) is now doing
   double duty -- it's both "how long counts as an intentional
   recording" and, below that, "counts as a confirm/toggle tap" -- so
   both behaviors need adjusting against how the button actually feels
   in hand. Also: the AI/record button is physically the same pin as the
   device's power button, which has its own hardware-level long-hold
   behavior independent of firmware — holding it for several seconds can
   trigger a hardware power cycle unrelated to recording. Keep test
   holds in the roughly 0.3-3 second range.
3. **Up/Down button wiring is unconfirmed on real hardware.**
   `BOARD_BUTTON_UP_PIN`/`BOARD_BUTTON_DOWN_PIN` (GPIO5/GPIO6) come from
   Seeed's own hardware-overview docs and an independent review, not
   from testing an actual device -- confirm the pins respond and that
   active-low-with-pullup is the right electrical convention (same as
   the AI button's) before relying on them.
4. **No touch, SD card, or sensor support wired up** — the board has them,
   this project doesn't use them (out of scope for the voice-note flow).
   Touch specifically: its I2C pins and address (SCL=GPIO2, SDA=GPIO3,
   INT=GPIO21, RST=GPIO41, EN=GPIO42, address 0x6A) are confirmed from
   the schematic, but the touch controller chip itself isn't named
   anywhere Seeed publishes -- likely bonded into the display module
   rather than a documented part on the mainboard -- so using it would
   need real trial-and-error protocol bring-up, not just wiring up a
   known driver.
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
