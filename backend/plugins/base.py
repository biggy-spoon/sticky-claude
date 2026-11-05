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
