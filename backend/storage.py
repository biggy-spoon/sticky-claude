"""SQLite-backed storage for conversation history, device status, and
mutable settings. SQLite because this backend is meant to run unattended on
a Pi/ZimaOS with zero extra setup -- no separate database server to install
or configure.
"""
import os
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from typing import Iterator, Optional

DB_PATH = os.getenv("DB_PATH", os.path.join(os.path.dirname(__file__), "sticky_claude.db"))

_SCHEMA = """
CREATE TABLE IF NOT EXISTS conversations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at TEXT NOT NULL,
    transcript TEXT NOT NULL,
    reply TEXT,
    success INTEGER NOT NULL,
    error_message TEXT
);

CREATE TABLE IF NOT EXISTS device_status (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    last_seen TEXT,
    last_ip TEXT
);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS todos (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at TEXT NOT NULL,
    text TEXT NOT NULL,
    done INTEGER NOT NULL DEFAULT 0,
    done_at TEXT
);
"""


@contextmanager
def _connect() -> Iterator[sqlite3.Connection]:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db() -> None:
    with _connect() as conn:
        conn.executescript(_SCHEMA)
        conn.execute("INSERT OR IGNORE INTO device_status (id, last_seen, last_ip) VALUES (1, NULL, NULL)")


def log_conversation(transcript: str, reply: Optional[str], success: bool,
                     error_message: Optional[str] = None) -> None:
    with _connect() as conn:
        conn.execute(
            "INSERT INTO conversations (created_at, transcript, reply, success, error_message) "
            "VALUES (?, ?, ?, ?, ?)",
            (datetime.now(timezone.utc).isoformat(), transcript, reply, int(success), error_message),
        )


def get_history(limit: int = 50) -> list[dict]:
    with _connect() as conn:
        rows = conn.execute(
            "SELECT id, created_at, transcript, reply, success, error_message "
            "FROM conversations ORDER BY id DESC LIMIT ?",
            (limit,),
        ).fetchall()
        return [dict(row) for row in rows]


def record_device_seen(ip: str) -> None:
    with _connect() as conn:
        conn.execute(
            "UPDATE device_status SET last_seen = ?, last_ip = ? WHERE id = 1",
            (datetime.now(timezone.utc).isoformat(), ip),
        )


def get_device_status() -> dict:
    with _connect() as conn:
        row = conn.execute("SELECT last_seen, last_ip FROM device_status WHERE id = 1").fetchone()
        return dict(row) if row else {"last_seen": None, "last_ip": None}


def get_setting(key: str, default: Optional[str] = None) -> Optional[str]:
    with _connect() as conn:
        row = conn.execute("SELECT value FROM settings WHERE key = ?", (key,)).fetchone()
        return row["value"] if row else default


def set_setting(key: str, value: str) -> None:
    with _connect() as conn:
        conn.execute(
            "INSERT INTO settings (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (key, value),
        )


def get_all_settings() -> dict:
    with _connect() as conn:
        rows = conn.execute("SELECT key, value FROM settings").fetchall()
        return {row["key"]: row["value"] for row in rows}


def add_todo(text: str) -> int:
    with _connect() as conn:
        cursor = conn.execute(
            "INSERT INTO todos (created_at, text, done) VALUES (?, ?, 0)",
            (datetime.now(timezone.utc).isoformat(), text),
        )
        return cursor.lastrowid


def list_todos(done: bool = False, limit: int = 50) -> list[dict]:
    # Open items oldest-first (a todo list reads in the order it was
    # built); done items most-recently-completed-first (for a "recently
    # finished" dashboard view).
    order = "DESC" if done else "ASC"
    with _connect() as conn:
        rows = conn.execute(
            f"SELECT id, created_at, text, done, done_at FROM todos "
            f"WHERE done = ? ORDER BY id {order} LIMIT ?",
            (int(done), limit),
        ).fetchall()
        return [dict(row) for row in rows]


def complete_todo(text_match: str) -> Optional[str]:
    """Marks the most recently-added open todo whose text contains
    `text_match` (case-insensitive) as done. Voice input won't carry a
    todo's numeric id, so matching is by text rather than id. Returns the
    matched todo's original text, or None if nothing open matched."""
    with _connect() as conn:
        row = conn.execute(
            "SELECT id, text FROM todos WHERE done = 0 AND text LIKE ? ORDER BY id DESC LIMIT 1",
            (f"%{text_match}%",),
        ).fetchone()
        if row is None:
            return None
        conn.execute(
            "UPDATE todos SET done = 1, done_at = ? WHERE id = ?",
            (datetime.now(timezone.utc).isoformat(), row["id"]),
        )
        return row["text"]


def complete_todo_by_id(todo_id: int) -> bool:
    """Same as complete_todo(), but by id rather than a text search --
    the device's Up/Down+tap interaction highlights a specific row and
    knows its id directly, unlike voice input. Returns False if there's
    no open todo with that id (already done, or never existed)."""
    with _connect() as conn:
        row = conn.execute(
            "SELECT id FROM todos WHERE id = ? AND done = 0", (todo_id,),
        ).fetchone()
        if row is None:
            return False
        conn.execute(
            "UPDATE todos SET done = 1, done_at = ? WHERE id = ?",
            (datetime.now(timezone.utc).isoformat(), todo_id),
        )
        return True
