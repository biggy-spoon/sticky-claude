"""The calendar plugin: create_calendar_event / list_calendar_events tools
backed by calendar_service.py's CalDAV/Google providers. This is the
reference implementation of plugins.base.Plugin -- a new plugin can
follow this file's shape.
"""
import logging
from datetime import datetime, timedelta, timezone
from typing import Optional

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import RedirectResponse

import calendar_service
import settings
from .base import DeviceView, DeviceViewItem, Plugin, SettingField

# Device screens are small and glanceable -- cap how many upcoming events
# get sent down for the on-device list rather than however many the
# calendar has in the next 30 days.
_DEVICE_VIEW_MAX_EVENTS = 15

log = logging.getLogger("sticky-claude.plugins.calendar")

_TOOLS = [
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


class CalendarPlugin(Plugin):
    @property
    def id(self) -> str:
        return "calendar"

    @property
    def name(self) -> str:
        return "Calendar"

    @property
    def description(self) -> str:
        return ("Create and check events by voice, via CalDAV (iCloud, Fastmail, Nextcloud, "
                "etc.) or Google Calendar.")

    def settings_schema(self) -> list[SettingField]:
        return [
            SettingField(
                key="calendar_provider", label="Calendar", type="select",
                options=[
                    {"value": "none", "label": "None"},
                    {"value": "caldav", "label": "CalDAV (iCloud, Fastmail, Nextcloud...)"},
                    {"value": "google", "label": "Google Calendar"},
                ],
            ),
            SettingField(key="caldav_url", label="CalDAV URL",
                        placeholder="https://caldav.icloud.com",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_username", label="Username",
                        placeholder="you@icloud.com", show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_password", label="App-specific password", type="password",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_calendar_name", label="Calendar name (optional)",
                        placeholder="Leave blank to use the first calendar",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="google_client_id", label="Google OAuth client ID",
                        show_if=("calendar_provider", "google")),
            SettingField(key="google_client_secret", label="Google OAuth client secret",
                        type="password", show_if=("calendar_provider", "google")),
            SettingField(key="google_calendar_id", label="Calendar ID (optional)",
                        placeholder="primary", show_if=("calendar_provider", "google")),
            SettingField(key="google_connect", label="", type="button",
                        button_label="Connect with Google",
                        button_url="/api/plugins/calendar/google/login",
                        show_if=("calendar_provider", "google")),
        ]

    def is_configured(self) -> bool:
        return calendar_service.get_provider() is not None

    def get_tools(self) -> list[dict]:
        return _TOOLS

    def execute_tool(self, name: str, tool_input: dict) -> str:
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
                return "\n".join(f"{e.start.isoformat()}: {e.summary}" for e in events)

            raise NotImplementedError(f"calendar plugin has no tool {name!r}")
        except Exception as exc:  # noqa: BLE001 -- surfaced to Claude as a tool result
            log.exception("Calendar tool %s failed", name)
            return f"Calendar action failed: {exc}"

    def dashboard_view(self) -> dict:
        provider = calendar_service.get_provider()
        if provider is None:
            return {"events": []}
        try:
            now = datetime.now(timezone.utc)
            events = provider.list_events(now, now + timedelta(days=30))
        except Exception as exc:  # noqa: BLE001 -- shown in the dashboard, not raised
            log.exception("Failed to load calendar events for dashboard view")
            return {"events": [], "error": str(exc)}
        return {"events": [{"summary": e.summary, "start": e.start.isoformat(),
                            "end": e.end.isoformat(), "description": e.description}
                           for e in events]}

    def device_view(self) -> Optional[DeviceView]:
        provider = calendar_service.get_provider()
        if provider is None:
            return None
        try:
            now = datetime.now(timezone.utc)
            events = provider.list_events(now, now + timedelta(days=30))
        except Exception:
            log.exception("Failed to load calendar events for device view")
            return None
        items = [
            DeviceViewItem(id=str(i), text=f"{e.start.strftime('%a %b %d, %I:%M %p')} {e.summary}")
            for i, e in enumerate(events[:_DEVICE_VIEW_MAX_EVENTS])
        ]
        return DeviceView(view_type="list", title="Calendar", items=items)

    def register_routes(self, app: FastAPI) -> None:
        @app.get("/api/plugins/calendar/google/login")
        def google_login(request: Request) -> RedirectResponse:
            config = settings.get_calendar_config()
            client_id = config.get("google_client_id")
            if not client_id:
                raise HTTPException(status_code=400, detail="Set Google client ID/secret first")
            redirect_uri = str(request.base_url) + "api/plugins/calendar/google/callback"
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

        @app.get("/api/plugins/calendar/google/callback")
        def google_callback(request: Request, code: str) -> dict:
            import requests

            config = settings.get_calendar_config()
            redirect_uri = str(request.base_url) + "api/plugins/calendar/google/callback"
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
                                    detail="Google did not return a refresh token -- revoke "
                                           "prior access in your Google Account and try again")
            settings.set("google_refresh_token", refresh_token)
            settings.set("calendar_provider", "google")
            return {"success": True, "message": "Google Calendar connected. You can close this tab."}
