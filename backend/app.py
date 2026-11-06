"""Voice-to-Claude proxy for the reTerminal Sticky, plus a small dashboard.

Receives a WAV recording from the device, transcribes it locally with
faster-whisper, sends the transcript to Claude (with whatever tools the
configured plugins contribute -- see plugins/ -- so it can actually act,
not just talk about acting), and returns the transcript + reply as JSON
for the device to render. The dashboard (see dashboard.html) exposes
conversation history, settings, device status, and each plugin's own
view over a small JSON API.
"""
import io
import logging
import os
from datetime import datetime, timezone

import anthropic
from anthropic import Anthropic
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from faster_whisper import WhisperModel

import plugin_hub
import plugins
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
    "You are a general-purpose voice assistant. The user can ask about anything, so don't "
    "assume a question is about you or the device you run on. Their words were transcribed "
    "from speech and may contain misheard words. Use web search for current events and for "
    "anything you aren't sure of, and give the answer itself rather than saying what you "
    "searched for. Your reply is shown as plain text on a small e-paper screen with no "
    "speaker, so keep it to 2-4 short sentences, with no markdown, lists or citations. "
    "Today's date is {today}. When a tool is available for what the user is asking, use "
    "it rather than just describing what you would do."
)

# Models that accept the newer web search tool. The model setting is free text,
# and an older model rejects the whole request if sent this version, so anything
# not listed here gets the basic one.
_WEB_SEARCH_20260209_MODELS = {
    "claude-fable-5-1", "claude-fable-5", "claude-opus-5-5", "claude-opus-5", "claude-opus-4-8",
    "claude-opus-4-7", "claude-opus-4-6", "claude-sonnet-5-5", "claude-sonnet-5",
    "claude-sonnet-4-6", "claude-haiku-5-5",
}


def _web_search_tool(model: str) -> dict:
    """Anthropic runs this tool itself, so there's no plugin behind it. max_uses
    limits how long a reply can take and what it can cost, since each search is
    billed."""
    version = "20260209" if model in _WEB_SEARCH_20260209_MODELS else "20250305"
    return {"type": f"web_search_{version}", "name": "web_search", "max_uses": 3}

app = FastAPI(title="sticky-claude backend")
whisper_model: WhisperModel | None = None

DASHBOARD_PATH = os.path.join(os.path.dirname(__file__), "dashboard.html")

for _plugin in plugins.all_plugins():
    _plugin.register_routes(app)


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


def _collect_tools() -> tuple[list[dict], dict[str, str]]:
    """Tool definitions from every configured plugin, plus a name -> plugin
    id map so a tool_use block can be routed back to whichever plugin
    owns it."""
    tool_defs: list[dict] = []
    tool_owner: dict[str, str] = {}
    for plugin in plugins.all_plugins():
        if not plugin.is_configured():
            continue
        for tool in plugin.get_tools():
            tool_defs.append(tool)
            tool_owner[tool["name"]] = plugin.id
    return tool_defs, tool_owner


def _execute_tool(name: str, tool_input: dict, tool_owner: dict[str, str]) -> str:
    plugin = plugins.get(tool_owner.get(name, ""))
    if plugin is None:
        return f"Unknown tool: {name}"
    return plugin.execute_tool(name, tool_input)


def _final_text(content: list) -> str:
    """The text after the last tool activity. Claude can write a line before a
    search, like "Let me look that up", which isn't part of the answer."""
    parts: list[str] = []
    for block in content:
        if block.type == "text":
            parts.append(block.text)
        elif block.type != "thinking":
            parts = []
    return "".join(parts).strip()


def _converse_with_claude(transcript: str, api_key: str, max_tool_iterations: int = 5) -> str:
    client = Anthropic(api_key=api_key)
    model = settings.get_claude_model()
    system_prompt = SYSTEM_PROMPT_TEMPLATE.format(today=datetime.now(timezone.utc).date().isoformat())
    tool_defs, tool_owner = _collect_tools()
    tool_defs.append(_web_search_tool(model))

    messages: list[dict] = [{"role": "user", "content": transcript}]

    for _ in range(max_tool_iterations):
        # Thinking counts towards max_tokens, so this is well above the reply length.
        response = client.messages.create(model=model, max_tokens=4096, system=system_prompt,
                                          messages=messages, tools=tool_defs)

        messages.append({"role": "assistant", "content": response.content})
        if response.stop_reason == "pause_turn":
            continue  # a long search paused; sending it back as is resumes it
        if response.stop_reason != "tool_use":
            return _final_text(response.content) or "I don't have an answer for that."

        tool_results = []
        for block in response.content:
            if block.type != "tool_use":
                continue
            result_text = _execute_tool(block.name, block.input, tool_owner)
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
    return status


@app.get("/api/settings")
def api_get_settings() -> dict:
    redacted = settings.redact({"anthropic_api_key": settings.get_anthropic_api_key()})
    return {
        "anthropic_api_key": redacted["anthropic_api_key"],
        "claude_model": settings.get_claude_model(),
    }


@app.post("/api/settings")
async def api_set_settings(request: Request) -> dict:
    body = await request.json()
    if body.get("anthropic_api_key"):
        settings.set("anthropic_api_key", body["anthropic_api_key"])
    if body.get("claude_model"):
        settings.set("claude_model", body["claude_model"])
    return {"success": True}


@app.get("/api/plugins")
def api_plugins() -> list[dict]:
    result = []
    for plugin in plugins.all_plugins():
        fields = []
        for field in plugin.settings_schema():
            value = None
            if field.type != "button":
                value = settings.get(field.key, "")
                if field.type == "password" and value:
                    value = f"...{value[-4:]}" if len(value) > 4 else "****"
            fields.append({
                "key": field.key, "label": field.label, "type": field.type,
                "placeholder": field.placeholder, "options": field.options, "value": value,
                "button_label": field.button_label, "button_url": field.button_url,
                "show_if": list(field.show_if) if field.show_if else None,
            })
        result.append({
            "id": plugin.id, "name": plugin.name, "description": plugin.description,
            "configured": plugin.is_configured(), "fields": fields,
        })
    return result


@app.post("/api/plugins/{plugin_id}/settings")
async def api_set_plugin_settings(plugin_id: str, request: Request) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    body = await request.json()
    field_keys = {field.key for field in plugin.settings_schema() if field.type != "button"}
    for key, value in body.items():
        # Skip blanks so an untouched password field doesn't overwrite a
        # previously-saved secret with an empty string.
        if key in field_keys and value:
            settings.set(key, value)
    return {"success": True}


@app.get("/api/plugins/{plugin_id}/view")
def api_plugin_view(plugin_id: str) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    return plugin.dashboard_view() or {}


@app.get("/api/plugin-hub")
def api_plugin_hub() -> dict:
    entries, error = plugin_hub.get_index()
    installed_ids = {plugin.id for plugin in plugins.all_plugins()}
    for entry in entries:
        entry["installed"] = entry.get("id") in installed_ids
    return {"plugins": entries, "error": error}


# --- Device screens (Up/Down button navigation on the ePaper panel) ----

@app.get("/api/device/views")
def api_device_views() -> dict:
    """Every configured plugin's own on-device screen, in a fixed order
    (plugin registration order), for the firmware to page through. The device's own home/conversation screen isn't
    included -- it's rendered locally by the firmware without a backend
    round-trip."""
    views = []
    for plugin in plugins.all_plugins():
        if not plugin.is_configured():
            continue
        view = plugin.device_view()
        if view is None:
            continue
        views.append({
            "plugin_id": plugin.id,
            "view_type": view.view_type,
            "title": view.title,
            "body": view.body,
            "items": [{"id": item.id, "text": item.text, "secondary": item.secondary,
                       "detail": item.detail, "checked": item.checked}
                      for item in (view.items or [])],
        })
    return {"views": views}


@app.post("/api/plugins/{plugin_id}/device-action")
async def api_plugin_device_action(plugin_id: str, request: Request) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    body = await request.json()
    item_id = body.get("item_id")
    if not item_id:
        raise HTTPException(status_code=400, detail="item_id is required")
    checked = body.get("checked")
    if not isinstance(checked, bool):
        raise HTTPException(status_code=400, detail="checked must be true or false")
    plugin.device_action(str(item_id), checked)
    return {"success": True}
