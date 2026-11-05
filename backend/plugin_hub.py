"""Fetches the community plugin-hub index (biggy-spoon/sticky-claude-plugin-hub)
so the dashboard can show what plugins exist beyond the ones already
installed. This only ever reads metadata -- it never downloads or runs a
plugin's code. Installing a plugin is a manual step (copy its files into
backend/plugins/ and restart), by design: see that repo's README for why.
"""
import logging
import time
from typing import Optional

import requests

log = logging.getLogger("sticky-claude.plugin_hub")

HUB_INDEX_URL = "https://raw.githubusercontent.com/biggy-spoon/sticky-claude-plugin-hub/main/index.json"
_CACHE_TTL_SECONDS = 15 * 60
_REQUEST_TIMEOUT_SECONDS = 5

_cache: Optional[list[dict]] = None
_cache_at: float = 0.0


def get_index(force: bool = False) -> tuple[list[dict], Optional[str]]:
    """Returns (entries, error). A fetch failure with a previously-good
    cache available returns the stale cache and no error, so a flaky
    connection doesn't make the dashboard's hub card flicker empty; only a
    failure with nothing cached yet surfaces an error."""
    global _cache, _cache_at
    if not force and _cache is not None and time.monotonic() - _cache_at < _CACHE_TTL_SECONDS:
        return _cache, None
    try:
        response = requests.get(HUB_INDEX_URL, timeout=_REQUEST_TIMEOUT_SECONDS)
        response.raise_for_status()
        entries = response.json()
        if not isinstance(entries, list):
            raise ValueError("hub index.json did not contain a JSON array")
        _cache, _cache_at = entries, time.monotonic()
        return entries, None
    except Exception as exc:
        log.warning("Could not fetch plugin hub index: %s", exc)
        if _cache is not None:
            return _cache, None
        return [], str(exc)
