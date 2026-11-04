# Architecture

## Hardware: reTerminal Sticky

- SoC: ESP32-S3R8 (dual-core, up to 240MHz, WiFi 4 + BLE 5.0), 8MB PSRAM, 32MB flash
- Display: 3.97" monochrome ePaper, 800x480, SSD1677 controller, SPI
- Audio: single PDM microphone (no speaker — this device is **text-out only**)
- AI/record button is the long-press trigger (push-to-talk)

Pin map lives in `firmware/components/board/include/board_config.h`,
confirmed against the
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) open-source
firmware for the same board (the pin assignments are a fact about how the
board is wired, not anyone's creative work, so reusing them doesn't carry
license obligations — only porting actual driver/business logic would).

## Why a backend proxy instead of calling Claude directly from the device

The ESP32-S3 can't run STT or an LLM itself. The device talks only to a
backend service on the user's own always-on Pi or ZimaOS box; that service
does local speech-to-text (faster-whisper) and the Claude API call, and
returns plain text for the device to show. This keeps the Anthropic API key
off the device entirely and keeps the device's only network dependency a
LAN address instead of a public cloud endpoint for STT.

## End-to-end flow

1. User holds the AI button → `button_service` fires press-start →
   `recording_service` arms the PDM mic and starts filling a PSRAM buffer.
2. User releases → `recording_service` stops, builds a 16kHz mono PCM16
   WAV in memory.
3. `assistant_service` POSTs the WAV to the backend's `/converse` endpoint.
4. Backend (`backend/app.py`) transcribes with faster-whisper, sends the
   transcript to Claude with a system prompt that keeps replies short
   (the display has no scrolling), and returns
   `{"transcript": ..., "reply": ...}`.
5. Device receives the reply and (once a font is wired up — see
   `firmware/README.md`) renders it on the ePaper panel.

## Open items

See `firmware/README.md` → "Known gaps" for the concrete list (font
rendering, ePaper bring-up verification, pre-roll buffering, button timing).
