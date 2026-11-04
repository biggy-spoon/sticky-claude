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

## API

- `GET /health` — model/config status.
- `POST /converse` — body is raw WAV bytes (16-bit PCM mono, see firmware's
  `recording_service`). Returns JSON `{"transcript": "...", "reply": "..."}`.

## Sizing note

`faster-whisper` on CPU is the main latency cost. On a Raspberry Pi 4/5,
`small` + `int8` is usually a few seconds for a 5-10s clip; ZimaOS hardware
with more cores will be faster. Tune `WHISPER_MODEL_SIZE` for your box.
