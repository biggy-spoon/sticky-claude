"""Voice-to-Claude proxy for the reTerminal Sticky, plus a small dashboard.

Receives a WAV recording from the device, transcribes it locally with
faster-whisper, sends the transcript to Claude (with calendar tools so it
can actually create/list events, not just talk about them), and returns
the transcript + reply as JSON for the device to render. The dashboard
(see dashboard.html) exposes conversation history, settings, device
status, and upcoming calendar events over a small JSON API.
"""
import io
import logging
import os
from datetime import datetime, timedelta, timezone

import anthropic
from anthropic import Anthropic
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from faster_whisper import WhisperModel

import calendar_service
import settings
import storage

load_dotenv()

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("sticky-claude")

# Whisper is loaded once at startup; changing these needs a restart.
WHISPER_MODEL_SIZE = os.getenv("WHISPER_MODEL_SIZE", "small")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
MAX_AUDIO_BYTES = 10 * 1024 * 1024  # generous ceiling; the device caps clips at ~10s

SYSTEM_PROMPT_TEMPLATE = (
    "You are a voice assistant replying to a short spoken note captured on a small "
    "e-paper sticky-note device with no speaker. Your reply is rendered as plain text "
    "on that screen, so keep it to 2-3 short sentences, no markdown, no lists. "
    "Today's date is {today}. When the calendar tools are available and the user asks "
    "to schedule, add, or check something on their calendar, use the tools rather than "
    "just describing what you would do."
)

CALENDAR_TOOLS = [
    {
        "name": "create_calendar_event",
        "description": "Add an event to the user's calendar.",
        "input_schema": {
            "type": "object",
            "properties": {
                "summary": {"type": "string", "description": "Short event title"},
                "start_time": {"type": "string",
                               "description": "ISO 8601 datetime, e.g. 2026-11-24T15:00:00"},
                "end_time": {"type": "string",
                             "description": "ISO 8601 datetime; defaults to 1 hour after "
                                            "start_time if omitted"},
                "description": {"type": "string", "description": "Optional extra detail"},
            },
            "required": ["summary", "start_time"],
        },
    },
    {
        "name": "list_calendar_events",
        "description": "List the user's calendar events within a date range.",
        "input_schema": {
            "type": "object",
            "properties": {
                "start_date": {"type": "string", "description": "ISO 8601 date, e.g. 2026-11-24"},
                "end_date": {"type": "string", "description": "ISO 8601 date"},
            },
            "required": ["start_date", "end_date"],
        },
    },
]

app = FastAPI(title="sticky-claude backend")
whisper_model: WhisperModel | None = None

DASHBOARD_PATH = os.path.join(os.path.dirname(__file__), "dashboard.html")


@app.on_event("startup")
def load_models() -> None:
    global whisper_model
    storage.init_db()
    log.info("Loading faster-whisper model=%s device=%s compute_type=%s",
              WHISPER_MODEL_SIZE, WHISPER_DEVICE, WHISPER_COMPUTE_TYPE)
    whisper_model = WhisperModel(WHISPER_MODEL_SIZE, device=WHISPER_DEVICE,
                                  compute_type=WHISPER_COMPUTE_TYPE)
    log.info("Startup complete")


@app.get("/health")
def health() -> dict:
    return {
        "status": "ok",
        "whisper_model": WHISPER_MODEL_SIZE,
        "claude_model": settings.get_claude_model(),
        "anthropic_configured": bool(settings.get_anthropic_api_key()),
    }


@app.post("/converse")
async def converse(request: Request) -> JSONResponse:
    if whisper_model is None:
        raise HTTPException(status_code=503, detail="Whisper model not loaded yet")
    api_key = settings.get_anthropic_api_key()
    if not api_key:
        raise HTTPException(status_code=500, detail="ANTHROPIC_API_KEY not configured")

    audio_bytes = await request.body()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty request body")
    if len(audio_bytes) > MAX_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail="Audio clip too large")

    if request.client is not None:
        storage.record_device_seen(request.client.host)

    try:
        transcript = _transcribe(audio_bytes)
    except Exception as exc:
        log.exception("Transcription failed")
        raise HTTPException(status_code=400, detail=f"Could not process audio: {exc}") from exc
    if not transcript.strip():
        raise HTTPException(status_code=422, detail="Could not transcribe any speech")

    try:
        reply = _converse_with_claude(transcript, api_key)
    except anthropic.APIError as exc:
        log.exception("Claude request failed")
        storage.log_conversation(transcript, None, success=False, error_message=str(exc))
        raise HTTPException(status_code=502, detail=f"Claude request failed: {exc}") from exc

    log.info("transcript=%r reply=%r", transcript, reply)
    storage.log_conversation(transcript, reply, success=True)
    return JSONResponse({"transcript": transcript, "reply": reply})


def _transcribe(wav_bytes: bytes) -> str:
    segments, _info = whisper_model.transcribe(io.BytesIO(wav_bytes), language="en")
    return " ".join(segment.text.strip() for segment in segments).strip()


def _execute_tool(name: str, tool_input: dict) -> str:
    provider = calendar_service.get_provider()
    if provider is None:
        return "Calendar is not configured yet -- tell the user to set it up in the dashboard."

    try:
        if name == "create_calendar_event":
            start = datetime.fromisoformat(tool_input["start_time"])
            end_raw = tool_input.get("end_time")
            end = datetime.fromisoformat(end_raw) if end_raw else start + timedelta(hours=1)
            provider.add_event(calendar_service.CalendarEvent(
                summary=tool_input["summary"], start=start, end=end,
                description=tool_input.get("description", ""),
            ))
            return f"Created '{tool_input['summary']}' at {start.isoformat()}."

        if name == "list_calendar_events":
            start = datetime.fromisoformat(tool_input["start_date"])
            end = datetime.fromisoformat(tool_input["end_date"])
            events = provider.list_events(start, end)
            if not events:
                return "No events found in that range."
            lines = [f"{event.start.isoformat()}: {event.summary}" for event in events]
            return "\n".join(lines)

        return f"Unknown tool: {name}"
    except Exception as exc:  # noqa: BLE001 -- surfaced to Claude as a tool result, not raised
        log.exception("Calendar tool %s failed", name)
        return f"Calendar action failed: {exc}"


def _converse_with_claude(transcript: str, api_key: str, max_tool_iterations: int = 3) -> str:
    client = Anthropic(api_key=api_key)
    model = settings.get_claude_model()
    system_prompt = SYSTEM_PROMPT_TEMPLATE.format(today=datetime.now(timezone.utc).date().isoformat())
    calendar_configured = calendar_service.get_provider() is not None
    tools = CALENDAR_TOOLS if calendar_configured else None

    messages: list[dict] = [{"role": "user", "content": transcript}]

    for _ in range(max_tool_iterations):
        kwargs = {"model": model, "max_tokens": 512, "system": system_prompt, "messages": messages}
        if tools:
            kwargs["tools"] = tools
        response = client.messages.create(**kwargs)

        if response.stop_reason != "tool_use":
            return "".join(block.text for block in response.content
                           if block.type == "text").strip()

        messages.append({"role": "assistant", "content": response.content})
        tool_results = []
        for block in response.content:
            if block.type != "tool_use":
                continue
            result_text = _execute_tool(block.name, block.input)
            tool_results.append({
                "type": "tool_result",
                "tool_use_id": block.id,
                "content": result_text,
            })
        messages.append({"role": "user", "content": tool_results})

    return "I'm having trouble finishing that request -- can you try again?"


# --- Dashboard ---------------------------------------------------------

@app.get("/dashboard")
def dashboard() -> FileResponse:
    return FileResponse(DASHBOARD_PATH)


@app.get("/api/history")
def api_history(limit: int = 50) -> list[dict]:
    return storage.get_history(limit=limit)


@app.get("/api/status")
def api_status() -> dict:
    status = storage.get_device_status()
    status["whisper_model"] = WHISPER_MODEL_SIZE
    status["claude_model"] = settings.get_claude_model()
    status["anthropic_configured"] = bool(settings.get_anthropic_api_key())
    status["calendar_configured"] = calendar_service.get_provider() is not None
    return status


@app.get("/api/settings")
def api_get_settings() -> dict:
    config = settings.get_calendar_config()
    config["anthropic_api_key"] = settings.get_anthropic_api_key()
    config["claude_model"] = settings.get_claude_model()
    return settings.redact(config)


@app.post("/api/settings")
async def api_set_settings(request: Request) -> dict:
    body = await request.json()
    if "anthropic_api_key" in body and body["anthropic_api_key"]:
        settings.set("anthropic_api_key", body["anthropic_api_key"])
    if "claude_model" in body and body["claude_model"]:
        settings.set("claude_model", body["claude_model"])
    settings.set_calendar_config(body)
    return {"success": True}


@app.get("/api/calendar/events")
def api_calendar_events(days: int = 30) -> list[dict]:
    provider = calendar_service.get_provider()
    if provider is None:
        return []
    now = datetime.now(timezone.utc)
    events = provider.list_events(now, now + timedelta(days=days))
    return [{"summary": e.summary, "start": e.start.isoformat(), "end": e.end.isoformat(),
            "description": e.description} for e in events]


@app.get("/api/calendar/google/login")
def api_google_login(request: Request) -> RedirectResponse:
    config = settings.get_calendar_config()
    client_id = config.get("google_client_id")
    if not client_id:
        raise HTTPException(status_code=400, detail="Set Google client ID/secret first")
    redirect_uri = str(request.base_url) + "api/calendar/google/callback"
    params = {
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "response_type": "code",
        "scope": "https://www.googleapis.com/auth/calendar",
        "access_type": "offline",
        "prompt": "consent",
    }
    query = "&".join(f"{key}={value}" for key, value in params.items())
    return RedirectResponse(f"https://accounts.google.com/o/oauth2/v2/auth?{query}")


@app.get("/api/calendar/google/callback")
def api_google_callback(request: Request, code: str) -> dict:
    import requests

    config = settings.get_calendar_config()
    redirect_uri = str(request.base_url) + "api/calendar/google/callback"
    response = requests.post("https://oauth2.googleapis.com/token", data={
        "client_id": config.get("google_client_id"),
        "client_secret": config.get("google_client_secret"),
        "code": code,
        "grant_type": "authorization_code",
        "redirect_uri": redirect_uri,
    }, timeout=15)
    response.raise_for_status()
    token_data = response.json()
    refresh_token = token_data.get("refresh_token")
    if not refresh_token:
        raise HTTPException(status_code=400,
                            detail="Google did not return a refresh token -- revoke prior "
                                   "access in your Google Account and try again")
    settings.set("google_refresh_token", refresh_token)
    settings.set("calendar_provider", "google")
    return {"success": True, "message": "Google Calendar connected. You can close this tab."}
