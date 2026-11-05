"""Provider-agnostic calendar access. Two providers so this works for
either major account type without picking a side:

- CalDAV: covers iCloud, Fastmail, Nextcloud, and any other CalDAV server.
  iCloud needs an app-specific password (appleid.apple.com -> Sign-In and
  Security -> App-Specific Passwords), not your normal Apple ID password.
- Google Calendar: via OAuth (see the /api/calendar/google/* routes in
  app.py for the one-time authorization flow).

Which one is active is just a setting (settings.get_calendar_config()),
so the dashboard can switch providers without a code change.
"""
import logging
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime
from typing import Optional

import settings

log = logging.getLogger("sticky-claude.calendar")


@dataclass
class CalendarEvent:
    summary: str
    start: datetime
    end: datetime
    description: str = ""


class CalendarProvider(ABC):
    @abstractmethod
    def add_event(self, event: CalendarEvent) -> None:
        ...

    @abstractmethod
    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        ...


class CalDavProvider(CalendarProvider):
    def __init__(self, url: str, username: str, password: str, calendar_name: str = ""):
        self._url = url
        self._username = username
        self._password = password
        self._calendar_name = calendar_name

    def _get_calendar(self):
        import caldav

        client = caldav.DAVClient(url=self._url, username=self._username,
                                  password=self._password)
        principal = client.principal()
        calendars = principal.calendars()
        if not calendars:
            raise RuntimeError("CalDAV account has no calendars")
        if self._calendar_name:
            for calendar in calendars:
                if calendar.name == self._calendar_name:
                    return calendar
            raise RuntimeError(f"No CalDAV calendar named {self._calendar_name!r}")
        return calendars[0]

    def add_event(self, event: CalendarEvent) -> None:
        from icalendar import Calendar as ICalendar
        from icalendar import Event as ICalEvent

        ical = ICalendar()
        ical.add("prodid", "-//sticky-claude//EN")
        ical.add("version", "2.0")
        vevent = ICalEvent()
        vevent.add("summary", event.summary)
        vevent.add("dtstart", event.start)
        vevent.add("dtend", event.end)
        if event.description:
            vevent.add("description", event.description)
        ical.add_component(vevent)

        calendar = self._get_calendar()
        calendar.save_event(ical.to_ical().decode("utf-8"))

    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        calendar = self._get_calendar()
        results = calendar.date_search(start=start, end=end)
        events: list[CalendarEvent] = []
        for result in results:
            for component in result.icalendar_instance.walk("VEVENT"):
                events.append(CalendarEvent(
                    summary=str(component.get("summary", "")),
                    start=component.get("dtstart").dt,
                    end=component.get("dtend").dt,
                    description=str(component.get("description", "")),
                ))
        return events


class GoogleCalendarProvider(CalendarProvider):
    def __init__(self, client_id: str, client_secret: str, refresh_token: str,
                 calendar_id: str = "primary"):
        self._client_id = client_id
        self._client_secret = client_secret
        self._refresh_token = refresh_token
        self._calendar_id = calendar_id or "primary"

    def _get_service(self):
        from google.oauth2.credentials import Credentials
        from googleapiclient.discovery import build

        creds = Credentials(
            token=None,
            refresh_token=self._refresh_token,
            client_id=self._client_id,
            client_secret=self._client_secret,
            token_uri="https://oauth2.googleapis.com/token",
        )
        return build("calendar", "v3", credentials=creds)

    def add_event(self, event: CalendarEvent) -> None:
        service = self._get_service()
        body = {
            "summary": event.summary,
            "description": event.description,
            "start": {"dateTime": event.start.isoformat()},
            "end": {"dateTime": event.end.isoformat()},
        }
        service.events().insert(calendarId=self._calendar_id, body=body).execute()

    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        service = self._get_service()
        response = service.events().list(
            calendarId=self._calendar_id,
            timeMin=start.isoformat(),
            timeMax=end.isoformat(),
            singleEvents=True,
            orderBy="startTime",
        ).execute()
        events: list[CalendarEvent] = []
        for item in response.get("items", []):
            start_raw = item["start"].get("dateTime", item["start"].get("date"))
            end_raw = item["end"].get("dateTime", item["end"].get("date"))
            events.append(CalendarEvent(
                summary=item.get("summary", ""),
                start=datetime.fromisoformat(start_raw),
                end=datetime.fromisoformat(end_raw),
                description=item.get("description", ""),
            ))
        return events


def get_provider() -> Optional[CalendarProvider]:
    config = settings.get_calendar_config()
    provider = config.get("calendar_provider", "none")

    if provider == "caldav":
        if not (config.get("caldav_url") and config.get("caldav_username")
                and config.get("caldav_password")):
            log.warning("CalDAV selected but not fully configured")
            return None
        return CalDavProvider(config["caldav_url"], config["caldav_username"],
                              config["caldav_password"], config.get("caldav_calendar_name", ""))

    if provider == "google":
        if not (config.get("google_client_id") and config.get("google_client_secret")
                and config.get("google_refresh_token")):
            log.warning("Google Calendar selected but not fully configured")
            return None
        return GoogleCalendarProvider(config["google_client_id"], config["google_client_secret"],
                                      config["google_refresh_token"],
                                      config.get("google_calendar_id", "primary"))

    return None
