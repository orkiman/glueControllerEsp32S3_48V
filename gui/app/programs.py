"""Persistent store of multiple named programs (config + 4 patterns).

A "program" is a complete operator profile (the same payload that
`profiles.to_dict` produces).  All programs live in a single JSON file in
the user's home directory so the GUI can remember the last one used and
offer a dropdown to switch between them.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from . import profiles
from .state import AppState

STORE_DIR  = Path.home() / ".glue_controller"
STORE_FILE = STORE_DIR / "programs.json"
SCHEMA_VERSION = 1
DEFAULT_NAME = "ברירת מחדל"


class ProgramStore:
    def __init__(self, path: Path = STORE_FILE) -> None:
        self.path = Path(path)
        self._programs: dict[str, dict[str, Any]] = {}
        self._last: str | None = None
        self.load()

    # ---- persistence -------------------------------------------------------
    def load(self) -> None:
        try:
            obj = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            obj = {}
        progs = obj.get("programs", {}) if isinstance(obj, dict) else {}
        if isinstance(progs, dict):
            self._programs = {str(k): v for k, v in progs.items()
                              if isinstance(v, dict)}
        last = obj.get("last") if isinstance(obj, dict) else None
        self._last = last if last in self._programs else None

    def _flush(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        obj = {
            "version":  SCHEMA_VERSION,
            "last":     self._last,
            "programs": self._programs,
        }
        self.path.write_text(
            json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")

    # ---- queries -----------------------------------------------------------
    def names(self) -> list[str]:
        return sorted(self._programs.keys())

    @property
    def last(self) -> str | None:
        return self._last

    def exists(self, name: str) -> bool:
        return name in self._programs

    # ---- mutations ---------------------------------------------------------
    def save_program(self, name: str, state: AppState) -> None:
        """Snapshot the current `state` under `name` and mark it as last."""
        self._programs[name] = profiles.to_dict(state)
        self._last = name
        self._flush()

    def delete(self, name: str) -> None:
        self._programs.pop(name, None)
        if self._last == name:
            self._last = None
        self._flush()

    def apply(self, name: str, state: AppState) -> bool:
        """Load program `name` into `state` (and push live). Returns False if
        the program does not exist."""
        obj = self._programs.get(name)
        if obj is None:
            return False
        profiles.apply_dict(obj, state)
        self._last = name
        self._flush()
        return True
