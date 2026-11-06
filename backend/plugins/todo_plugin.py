"""The todo-list plugin: add_todo / list_todos / complete_todo tools,
backed directly by storage.py (plain local SQLite, no external service
and no settings -- it works out of the box). One of the two default
plugins shipped with the backend, alongside calendar_plugin.py.
"""
import logging
from datetime import datetime
from typing import Optional
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import settings
import storage
from .base import DeviceView, DeviceViewItem, Plugin, SettingField

log = logging.getLogger("sticky-claude.plugins.todo")

_TOOLS = [
    {
        "name": "add_todo",
        "description": "Add an item to the user's todo list.",
        "input_schema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "What to do, e.g. 'buy milk'"},
            },
            "required": ["text"],
        },
    },
    {
        "name": "list_todos",
        "description": "List the user's open (not yet completed) todo items.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "complete_todo",
        "description": "Mark a todo item as done, identified by its text.",
        "input_schema": {
            "type": "object",
            "properties": {
                "text": {"type": "string",
                         "description": "The todo's text, or a distinctive part of it"},
            },
            "required": ["text"],
        },
    },
]


def _start_of_today() -> datetime:
    name = settings.get("todo_timezone", "")
    try:
        now = datetime.now(ZoneInfo(name)) if name else datetime.now().astimezone()
    except (ZoneInfoNotFoundError, ValueError):
        log.warning("Unknown todo_timezone %r, using the server's timezone", name)
        now = datetime.now().astimezone()
    return now.replace(hour=0, minute=0, second=0, microsecond=0)


class TodoPlugin(Plugin):
    @property
    def id(self) -> str:
        return "todo"

    @property
    def name(self) -> str:
        return "Todo list"

    @property
    def description(self) -> str:
        return "A voice-driven todo list -- add, list, and complete items by speaking."

    def settings_schema(self) -> list[SettingField]:
        return [SettingField(
            key="todo_timezone", label="Timezone for the daily reset",
            placeholder="e.g. America/New_York (blank uses the server's timezone)",
        )]

    def is_configured(self) -> bool:
        return True

    def get_tools(self) -> list[dict]:
        return _TOOLS

    def execute_tool(self, name: str, tool_input: dict) -> str:
        try:
            if name == "add_todo":
                storage.add_todo(tool_input["text"])
                return f"Added '{tool_input['text']}' to the todo list."

            if name == "list_todos":
                todos = storage.list_todos()
                if not todos:
                    return "The todo list is empty."
                return "\n".join(f"- {t['text']}" for t in todos)

            if name == "complete_todo":
                matched = storage.complete_todo(tool_input["text"])
                if matched is None:
                    return f"Couldn't find an open todo matching '{tool_input['text']}'."
                return f"Marked '{matched}' as done."

            raise NotImplementedError(f"todo plugin has no tool {name!r}")
        except Exception as exc:  # noqa: BLE001 -- surfaced to Claude as a tool result
            log.exception("Todo tool %s failed", name)
            return f"Todo action failed: {exc}"

    def dashboard_view(self) -> dict:
        return {
            "open": storage.list_todos(done=False),
            "done": storage.list_todos(done=True, limit=10),
        }

    def device_view(self) -> Optional[DeviceView]:
        # Completed items stay visible, struck through at the bottom, until the
        # day ends. Unfinished items carry over.
        items = [DeviceViewItem(id=str(t["id"]), text=t["text"], checked=False)
                 for t in storage.list_todos(done=False)]
        items += [DeviceViewItem(id=str(t["id"]), text=t["text"], checked=True)
                  for t in storage.list_todos_done_since(_start_of_today())]
        return DeviceView(view_type="list", title="Todo list", items=items)

    def device_action(self, item_id: str, checked: bool) -> None:
        try:
            todo_id = int(item_id)
        except ValueError:
            log.warning("device_action got a non-numeric todo id: %r", item_id)
            return
        storage.set_todo_done(todo_id, checked)
