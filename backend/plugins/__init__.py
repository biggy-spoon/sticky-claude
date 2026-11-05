"""Plugin registry. Import this module to get the built-in plugins
registered; `app.py` does this once at import time.

Adding a new built-in plugin: write it (see calendar_plugin.py as the
reference implementation of base.Plugin), then add one line below.
Third-party/local plugins that aren't part of this repo can call
`register()` themselves from anywhere imported before startup.
"""
from typing import Optional

from .base import Plugin

_plugins: dict[str, Plugin] = {}


def register(plugin: Plugin) -> None:
    _plugins[plugin.id] = plugin


def all_plugins() -> list[Plugin]:
    return list(_plugins.values())


def get(plugin_id: str) -> Optional[Plugin]:
    return _plugins.get(plugin_id)


def _register_built_ins() -> None:
    from .calendar_plugin import CalendarPlugin
    register(CalendarPlugin())


_register_built_ins()
