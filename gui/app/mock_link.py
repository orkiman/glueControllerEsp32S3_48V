"""In-process fake firmware for UI development without hardware.

Accepts the same NDJSON commands and emits plausible status / ack / event
traffic so the GUI can be developed and demoed end-to-end.
"""
from __future__ import annotations

import copy
import random
from typing import Any

from PySide6.QtCore import QTimer

from .link_base import LinkBase


class MockLink(LinkBase):
    def __init__(self) -> None:
        super().__init__()
        self._active = False
        self._fault = False
        self._sheet_count = 0
        self._speed = 0.0
        self._target_speed = 480.0  # mm/s
        self._status_timer = QTimer(self)
        self._status_timer.setInterval(200)
        self._status_timer.timeout.connect(self._emit_status)
        self._sheet_timer = QTimer(self)
        self._sheet_timer.setInterval(700)
        self._sheet_timer.timeout.connect(self._emit_sheet)
        # Shared state the real controller keeps (config, patterns, programs).
        self._config: dict[str, Any] = {
            "pulses_per_mm": 1.0, "min_speed_mm_s": 100.0,
            "photocell_offset_mm": 250.0, "debounce_ms": 20,
            "pick_current_a": 1.0, "hold_current_a": 0.4, "encoder_source": 0,
        }
        self._patterns: list[dict[str, Any]] = [
            {"type": "none", "elements": [], "on_timeout_ms": 1.2}
            for _ in range(4)
        ]
        self._programs: dict[int, dict[str, Any]] = {}
        self._active_id = 1
        self._programs[1] = self._snapshot("ברירת מחדל")

    # ---- LinkBase API -------------------------------------------------------
    def open(self, *_a: Any, **_kw: Any) -> None:  # noqa: A003
        self.start()

    def close(self) -> None:
        self._status_timer.stop()
        self._sheet_timer.stop()
        self._set_connected(False, "mock closed")

    def send(self, payload: dict[str, Any]) -> None:
        cmd = payload.get("cmd", "")
        if cmd == "ping":
            self._ack("ping")
        elif cmd == "set_active":
            self._active = bool(payload.get("active", False))
            if self._active:
                self._fault = False
                self._sheet_timer.start()
            else:
                self._sheet_timer.stop()
                self._speed = 0.0
            self._ack("set_active")
        elif cmd == "set_config":
            for k in self._config:
                if k in payload:
                    self._config[k] = payload[k]
            self._ack("set_config")
        elif cmd == "set_pattern":
            gun = int(payload.get("gun", 0))
            if 1 <= gun <= 4:
                self._patterns[gun - 1] = {
                    "type": payload.get("type", "none"),
                    "elements": list(payload.get("elements", [])),
                    "on_timeout_ms": payload.get("on_timeout_ms", 1.2),
                }
            self._ack("set_pattern")
        elif cmd == "get_state":
            self._emit_state()
            self._ack("get_state")
        elif cmd == "list_programs":
            self._emit_programs()
            self._ack("list_programs")
        elif cmd == "save_program":
            pid = int(payload.get("id", 0)) or (max(self._programs, default=0) + 1)
            self._programs[pid] = self._snapshot(str(payload.get("name", "")))
            self._active_id = pid
            self._emit_programs()
            self._ack("save_program")
        elif cmd == "load_program":
            prog = self._programs.get(int(payload.get("id", 0)))
            if prog is None:
                self._error("load_program", "load_failed")
                return
            self._active_id = int(payload["id"])
            self._config = copy.deepcopy(prog["config"])
            self._patterns = copy.deepcopy(prog["patterns"])
            self._emit_state()
            self._ack("load_program")
        elif cmd == "rename_program":
            prog = self._programs.get(int(payload.get("id", 0)))
            if prog is None:
                self._error("rename_program", "rename_failed")
                return
            prog["name"] = str(payload.get("name", ""))
            self._emit_programs()
            self._ack("rename_program")
        elif cmd == "delete_program":
            if self._programs.pop(int(payload.get("id", 0)), None) is None:
                self._error("delete_program", "delete_failed")
                return
            if not self._programs:
                self._programs[1] = self._snapshot("Default")
            if self._active_id not in self._programs:
                self._active_id = min(self._programs)
            self._emit_state()
            self._ack("delete_program")
        elif cmd == "test_open":
            self._ack("test_open")
        elif cmd == "test_close":
            self._ack("test_close")
        elif cmd == "calib_arm":
            self._ack("calib_arm")
            # Simulate a calibration result after a moment.
            QTimer.singleShot(800, lambda: self.event_received.emit({
                "event": "calib_result",
                "pulses_per_mm": 12.34 + random.uniform(-0.05, 0.05),
            }))
        elif cmd == "sw_trigger":
            self._ack("sw_trigger")
        else:
            self.event_received.emit({"event": "error",
                                      "cmd": cmd, "reason": "unknown_cmd"})

    # ---- lifecycle ----------------------------------------------------------
    def start(self) -> None:
        self._set_connected(True, "mock")
        self._status_timer.start()

    # ---- internals ----------------------------------------------------------
    def _ack(self, cmd: str) -> None:
        self.event_received.emit({"event": "ack", "cmd": cmd})

    def _error(self, cmd: str, reason: str) -> None:
        self.event_received.emit({"event": "error", "cmd": cmd, "reason": reason})

    def _snapshot(self, name: str) -> dict[str, Any]:
        return {"name": name, "config": copy.deepcopy(self._config),
                "patterns": copy.deepcopy(self._patterns)}

    def _emit_programs(self) -> None:
        self.event_received.emit({
            "event": "programs_list", "active_id": self._active_id,
            "programs": [{"id": pid, "name": p["name"]}
                         for pid, p in sorted(self._programs.items())],
        })

    def _emit_state(self) -> None:
        self.event_received.emit({"event": "config", **self._config})
        for i, p in enumerate(self._patterns):
            self.event_received.emit({"event": "pattern", "gun": i + 1,
                                      **copy.deepcopy(p)})
        self._emit_programs()

    def _emit_status(self) -> None:
        if self._active and not self._fault:
            # Wobble around target.
            self._speed += (self._target_speed - self._speed) * 0.2
            self._speed += random.uniform(-5, 5)
        else:
            self._speed *= 0.5
        self.event_received.emit({
            "event": "status",
            "active": self._active,
            "fault": self._fault,
            "speed_mm_s": round(self._speed, 1),
            "sheet_count": self._sheet_count,
            "queue_depth": 0,
        })

    def _emit_sheet(self) -> None:
        if not (self._active and not self._fault):
            return
        self._sheet_count += 1
        self.event_received.emit({
            "event": "pattern_event",
            "kind": "sheet_detected",
            "sheet_count": self._sheet_count,
        })
