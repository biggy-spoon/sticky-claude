# sticky-claude firmware

ESP-IDF (C++) firmware for the Seeed Studio reTerminal Sticky (ESP32-S3R8).
Hold the AI/record button, speak, release — the clip gets POSTed to the
`backend` service, which transcribes it and asks Claude, and the reply comes
back as JSON.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
v5.x (this was written against the v5 I2S PDM RX API). Not built or
flashed in this session — there was no ESP-IDF toolchain available to verify
it compiles. Treat this as a first draft that needs real bring-up.

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig   # under "sticky-claude": set WiFi SSID/password and the backend URL
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Architecture

| Component | Role |
| --- | --- |
| `board` | GPIO/SPI pin map for the button, PDM mic, and ePaper panel |
| `button_service` | Polls the AI button, debounced; press-start/press-end callbacks |
| `pdm_mic` | I2S PDM RX capture, 16kHz mono 16-bit |
| `recording_service` | PSRAM clip buffer + WAV encoding |
| `wifi_service` | Blocking WiFi STA connect |
| `assistant_service` | POSTs the WAV to the backend, parses `{transcript, reply}` |
| `epaper_display` | SPI transport + SSD1677 command set + 1bpp framebuffer |
| `main` | Wires button hold → record → upload → (eventually) display |

## Known gaps

This is a first pass, not a finished product — these are the pieces that
need real hardware time:

1. **Text rendering is not implemented.** `main.cpp` only draws a black
   status bar while busy, as a placeholder. To actually show Claude's reply
   you need a font. Recommended: pull in
   [u8g2](https://github.com/olikraus/u8g2) (BSD-licensed) as a managed
   component and drive it via its custom-display callback against
   `epaper_display`'s framebuffer, rather than hand-writing glyph bitmaps.
2. **The ePaper init/refresh command sequence is unverified.** It follows
   the standard SSD16xx opcode set but was written without a real panel to
   test against — expect to debug command order, timing, or the border
   waveform byte. If it doesn't light up, compare against
   [the `folloup-sticky` project's `ssd1677_driver.cpp`](https://github.com/alxv2016/folloup-sticky/blob/main/components/epaper_panel/ssd1677_driver.cpp),
   which targets the same panel/controller on the same board and is known
   to work — note that project is **GPLv3**, so directly porting its code
   (rather than just reading it for reference) would mean this component
   needs to be GPLv3 too.
3. **No pre-roll buffering.** Recording starts exactly when the capture
   task notices the button is down, so the first ~20-50ms of speech right
   at press can get clipped. Followup's `recording_service` solves this
   with a continuously-filled ring buffer armed ahead of the long-press
   threshold; worth porting that pattern if clipped first words become
   annoying in practice.
4. **Button timing not tuned.** `kMinHoldMs` (250ms) is a guess to filter
   accidental taps; adjust against how the button actually feels in hand.
5. **No touch, SD card, or sensor support wired up** — the board has them,
   this project doesn't use them (out of scope for the voice-note flow).
