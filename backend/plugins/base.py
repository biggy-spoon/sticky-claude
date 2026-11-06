"""The plugin contract. A plugin bundles everything needed to give Claude
a new capability: the tool definitions it contributes, how to execute
them, what settings it needs, and (optionally) extra HTTP routes it needs
for its own setup flow (e.g. an OAuth callback).

The dashboard renders every plugin's settings form generically from
`settings_schema()` -- a new plugin that only needs text/password fields
needs zero dashboard frontend changes. `calendar_plugin.py` is the
reference implementation; read it alongside this file.
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Optional

from fastapi import FastAPI


@dataclass
class SettingField:
    """One field in a plugin's settings form, rendered generically by the
    dashboard. Keep field `key`s globally unique (prefix with the plugin
    id if there's any chance of collision) -- they're flat keys in the
    shared settings table.
    """
    key: str
    label: str
    type: str = "text"  # "text" | "password" | "select" | "button"
    placeholder: str = ""
    options: Optional[list[dict]] = None  # for type="select": [{"value": ..., "label": ...}]
    # For type="button": a label and a URL to open (e.g. an OAuth login
    # redirect). The button itself doesn't carry a settings value.
    button_label: Optional[str] = None
    button_url: Optional[str] = None
    # Only show this field when another field (by key) currently equals
    # `show_if[1]`. Lets a plugin have provider-specific sub-fields
    # without the dashboard needing to know anything calendar-specific.
    show_if: Optional[tuple[str, str]] = None


@dataclass
class ToolResult:
    text: str


@dataclass
class DeviceViewItem:
    """One row in a device-rendered list view (base.DeviceView, below).
    `checked` is None for a plain (non-interactive) row like a calendar
    event; True/False for a checkable row like a todo, which a tap on the
    device toggles.
    """
    id: str
    text: str
    checked: Optional[bool] = None
    secondary: str = ""  # optional smaller second line, e.g. an event's date
    detail: str = ""  # optional full text; tapping the row opens it on its own screen


@dataclass
class DeviceView:
    """A plugin's own on-device screen, shown on the ePaper display when
    the user pages to it with the device's buttons or by swiping. Deliberately
    a small declarative payload -- like SettingField for the web
    dashboard's forms -- rather than letting a plugin send anything it
    wants: the firmware only ever renders one of a few known
    `view_type`s, so a plugin (including a third-party one from the
    plugin hub) can describe a screen but never execute code on the
    device.
    """
    view_type: str  # "text" | "list"
    title: str
    body: Optional[str] = None                    # view_type == "text"
    items: Optional[list[DeviceViewItem]] = None   # view_type == "list"


class Plugin(ABC):
    @property
    @abstractmethod
    def id(self) -> str:
        """Unique slug, e.g. 'calendar'. Used in URLs and setting-key prefixes."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Human-readable name for the dashboard."""

    @property
    def description(self) -> str:
        return ""

    def settings_schema(self) -> list[SettingField]:
        """Fields the dashboard should render for this plugin. Empty list
        if the plugin needs no configuration."""
        return []

    @abstractmethod
    def is_configured(self) -> bool:
        """Whether this plugin has enough settings to actually do anything."""

    def get_tools(self) -> list[dict]:
        """Claude tool definitions (Anthropic's `tools` format) this
        plugin contributes. Only called when is_configured() is True."""
        return []

    def execute_tool(self, name: str, tool_input: dict) -> str:
        """Execute one of this plugin's tools and return the tool_result
        text. Only called for tool names this plugin's get_tools()
        returned -- raise NotImplementedError if that invariant is ever
        broken (it's a bug in the registry, not the caller)."""
        raise NotImplementedError(f"{self.id} does not implement tool {name!r}")

    def register_routes(self, app: FastAPI) -> None:
        """Optional: mount plugin-specific HTTP routes under
        /api/plugins/<id>/... (e.g. an OAuth callback). Default: none."""
        return None

    def dashboard_view(self) -> Optional[dict]:
        """Optional: extra data for a plugin-specific dashboard card,
        e.g. calendar's upcoming-events list. Return None if the plugin
        has no dashboard view beyond its settings form."""
        return None

    def device_view(self) -> Optional[DeviceView]:
        """Optional: this plugin's own screen on the device's ePaper
        display, shown when the user pages to it with the buttons or by
        swiping. Return None for no device screen at all (the plugin is
        skipped in the on-device rotation) -- the default, so a plugin
        with no use for a device screen needs to do nothing here."""
        return None

    def device_action(self, item_id: str, checked: bool) -> None:
        """Optional: called when the user taps a checkable row (by its
        DeviceViewItem.id) on the device, with the state it should now have.
        Only called for items this plugin's device_view() returned with
        `checked` set to True/False. Default: no-op, for a plugin whose device
        screen is read-only."""
        return None
