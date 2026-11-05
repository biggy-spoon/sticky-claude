"""Plugin registry. Every .py file in this package (other than base.py) is
scanned at import time for a plugins.base.Plugin subclass defined in it;
any found are instantiated and registered automatically.

This auto-discovery *is* the "install" story for a community plugin: copy
its file(s) into this directory and restart the backend -- no registry
edits needed. It's also exactly how the two built-in plugins
(calendar_plugin.py, todo_plugin.py) get registered; they aren't special-
cased, which is deliberate proof the discovery mechanism is what actually
runs in production, not just what community plugins get.

See base.py for the plugin contract and calendar_plugin.py / todo_plugin.py
for reference implementations.
"""
import importlib
import inspect
import logging
import pkgutil
from typing import Optional

from .base import Plugin

log = logging.getLogger("sticky-claude.plugins")

_plugins: dict[str, Plugin] = {}


def register(plugin: Plugin) -> None:
    _plugins[plugin.id] = plugin


def all_plugins() -> list[Plugin]:
    return list(_plugins.values())


def get(plugin_id: str) -> Optional[Plugin]:
    return _plugins.get(plugin_id)


def _discover() -> None:
    for module_info in pkgutil.iter_modules(__path__):
        if module_info.name == "base":
            continue
        try:
            module = importlib.import_module(f".{module_info.name}", package=__name__)
        except Exception:
            log.exception("Failed to import plugin module %r -- skipping it", module_info.name)
            continue
        for _, obj in inspect.getmembers(module, inspect.isclass):
            # obj.__module__ check excludes names merely imported into this
            # module (e.g. `from .base import Plugin`) -- only classes
            # actually defined here should be registered.
            if obj is Plugin or not issubclass(obj, Plugin) or obj.__module__ != module.__name__:
                continue
            try:
                register(obj())
            except Exception:
                log.exception("Failed to load plugin %s from %r -- skipping it",
                               obj.__name__, module_info.name)


_discover()
