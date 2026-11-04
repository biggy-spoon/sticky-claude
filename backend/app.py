"""Voice-to-Claude proxy for the reTerminal Sticky.

Receives a WAV recording from the device, transcribes it locally with
faster-whisper, sends the transcript to Claude, and returns both the
transcript and the reply as JSON for the device to render.
"""
import io
import logging
import os

from anthropic import Anthropic
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from faster_whisper import WhisperModel

load_dotenv()

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("sticky-claude")

WHISPER_MODEL_SIZE = os.getenv("WHISPER_MODEL_SIZE", "small")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
CLAUDE_MODEL = os.getenv("CLAUDE_MODEL", "claude-haiku-5-5")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY")
MAX_AUDIO_BYTES = 10 * 1024 * 1024  # generous ceiling; the device caps clips at ~10s

SYSTEM_PROMPT = (
    "You are a voice assistant replying to a short spoken note captured on a small "
    "e-paper sticky-note device with no speaker. Your reply is rendered as plain text "
    "on that screen, so keep it to 2-3 short sentences, no markdown, no lists."
)

if not ANTHROPIC_API_KEY:
    log.warning("ANTHROPIC_API_KEY is not set; /converse will fail until it is configured")

app = FastAPI(title="sticky-claude backend")
whisper_model: WhisperModel | None = None
anthropic_client: Anthropic | None = None


@app.on_event("startup")
def load_models() -> None:
    global whisper_model, anthropic_client
    log.info("Loading faster-whisper model=%s device=%s compute_type=%s",
              WHISPER_MODEL_SIZE, WHISPER_DEVICE, WHISPER_COMPUTE_TYPE)
    whisper_model = WhisperModel(WHISPER_MODEL_SIZE, device=WHISPER_DEVICE,
                                  compute_type=WHISPER_COMPUTE_TYPE)
    if ANTHROPIC_API_KEY:
        anthropic_client = Anthropic(api_key=ANTHROPIC_API_KEY)
    log.info("Startup complete")


@app.get("/health")
def health() -> dict:
    return {
        "status": "ok",
        "whisper_model": WHISPER_MODEL_SIZE,
        "claude_model": CLAUDE_MODEL,
        "anthropic_configured": anthropic_client is not None,
    }


@app.post("/converse")
async def converse(request: Request) -> JSONResponse:
    if whisper_model is None:
        raise HTTPException(status_code=503, detail="Whisper model not loaded yet")
    if anthropic_client is None:
        raise HTTPException(status_code=500, detail="ANTHROPIC_API_KEY not configured")

    audio_bytes = await request.body()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty request body")
    if len(audio_bytes) > MAX_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail="Audio clip too large")

    transcript = _transcribe(audio_bytes)
    if not transcript.strip():
        raise HTTPException(status_code=422, detail="Could not transcribe any speech")

    reply = _ask_claude(transcript)
    log.info("transcript=%r reply=%r", transcript, reply)
    return JSONResponse({"transcript": transcript, "reply": reply})


def _transcribe(wav_bytes: bytes) -> str:
    segments, _info = whisper_model.transcribe(io.BytesIO(wav_bytes), language="en")
    return " ".join(segment.text.strip() for segment in segments).strip()


def _ask_claude(transcript: str) -> str:
    message = anthropic_client.messages.create(
        model=CLAUDE_MODEL,
        max_tokens=256,
        system=SYSTEM_PROMPT,
        messages=[{"role": "user", "content": transcript}],
    )
    return "".join(block.text for block in message.content if block.type == "text").strip()
