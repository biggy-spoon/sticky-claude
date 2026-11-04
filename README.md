# sticky-claude

Push-to-talk voice assistant on a [Seeed Studio reTerminal Sticky](https://www.seeedstudio.com/sticky/),
proxied through a home server to Claude.

Hold the device's AI button, speak, release. The ESP32-S3 records the clip
and uploads it over WiFi to a small backend service running on an always-on
Pi or ZimaOS box; that service transcribes the audio locally and asks Claude
for a reply, which comes back to the device to display.

```
[reTerminal Sticky] --WAV upload--> [backend on Pi/ZimaOS] --text--> [Claude API]
        ^                                    |
        |__________________text reply________|
```

- `firmware/` — ESP-IDF firmware for the device (button, mic, WiFi, ePaper).
- `backend/` — FastAPI proxy: local Whisper transcription + Claude call.
- `docs/ARCHITECTURE.md` — hardware details and design notes.

## Status

Early scaffold, not yet flashed or run end-to-end. See
`firmware/README.md`'s "Known gaps" for what's stubbed vs. real, in
particular: **text rendering on the display isn't implemented yet** (no
font wired up), and the ePaper panel's init sequence hasn't been verified
against real hardware.

## License

MIT (see `LICENSE`), except where noted — the firmware's `epaper_display`
component was written from the public SSD16xx command set, not ported from
any GPL codebase, so it's MIT too. If you later port code from the
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) project
(GPLv3) to fix display bring-up, that ported file would need to stay GPLv3.
