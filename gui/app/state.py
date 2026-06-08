"""Central application state.

The UI binds to signals here; commands flow back through `send_*` methods
which write to the link. Live publish — no Apply button. UI changes call
`set_*` methods which immediately marshal to set_config / set_pattern.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from PySide6.QtCore import QObject, QTimer, Signal

from . import protocol as proto
from .link_base import LinkBase


@dataclass
class GunPattern:
    type: proto.PatternType = proto.PatternType.NONE
    elements: list[proto.PatternElement] = field(default_factory=list)
    # Per-gun total on-time budget (ms) measured from the start of Phase 1.
    # Dots mode: equals the droplet on-time. Lines mode: long safety ceiling
    # only -- line ends are encoder-position driven by PatternScheduler.
    on_timeout_ms: float = 1.2


@dataclass
class RuntimeConfig:
    pulses_per_mm: float       = 1.0
    min_speed_mm_s: float      = 100.0
    photocell_offset_mm: float = 250.0
    debounce_ms: int           = 20
    pick_current_a: float      = 1.0
    hold_current_a: float      = 0.4


@dataclass
class LiveStatus:
    active: bool      = False
    fault: bool       = False
    speed_mm_s: float = 0.0
    sheet_count: int  = 0
    queue_depth: int  = 0


class AppState(QObject):
    # ---- signals consumed by widgets ----------------------------------------
    status_changed     = Signal(object)   # LiveStatus
    config_changed     = Signal(object)   # RuntimeConfig
    pattern_changed    = Signal(int, object)  # (gun_index_0based, GunPattern)
    log_appended       = Signal(dict)
    command_sent       = Signal(dict)      # outbound command payload
    connection_changed = Signal(bool, str)
    error_received     = Signal(str, str)  # (cmd, reason)

    def __init__(self, link: LinkBase) -> None:
        super().__init__()
        self.link = link
        self.config   = RuntimeConfig()
        self.patterns = [GunPattern() for _ in range(proto.NUM_GUNS)]
        self.status   = LiveStatus()

        link.event_received.connect(self._on_event)
        link.connection_changed.connect(self._on_link_conn)

        # Ping every 1s so the firmware watchdog stays fed and we measure RTT.
        self._ping_timer = QTimer(self)
        self._ping_timer.setInterval(1000)
        self._ping_timer.timeout.connect(self._ping)
        self._ping_timer.start()

    # ---- outbound (UI -> firmware) -----------------------------------------
    def _send(self, payload: dict[str, Any]) -> None:
        """Single funnel for outbound traffic so we can surface every command
        in the event log (the 1 Hz ping keep-alive is deliberately silent)."""
        if payload.get("cmd") != "ping":
            self.command_sent.emit(payload)
        self.link.send(payload)

    def set_active(self, active: bool) -> None:
        self._send(proto.cmd_set_active(active))

    def push_config(self, **fields: float) -> None:
        """Apply local edits then publish them live."""
        for k, v in fields.items():
            if hasattr(self.config, k):
                setattr(self.config, k, type(getattr(self.config, k))(v))
        self.config_changed.emit(self.config)
        self._send(proto.cmd_set_config(**fields))

    def push_pattern(self, gun_index_0based: int) -> None:
        gp = self.patterns[gun_index_0based]
        if gp.type == proto.PatternType.NONE:
            return  # firmware refuses 'none'; clear via empty 'lines'/'dots'.
        self._send(proto.cmd_set_pattern(
            gun_1based=gun_index_0based + 1,
            ptype=gp.type,
            elements=gp.elements,
            on_timeout_ms=gp.on_timeout_ms,
        ))
        self.pattern_changed.emit(gun_index_0based, gp)

    def test_open(self, gun_1based: int, timeout_ms: int = 1000) -> None:
        self._send(proto.cmd_test_open(gun_1based, timeout_ms))

    def test_close(self, gun_1based: int = 0) -> None:
        self._send(proto.cmd_test_close(gun_1based))

    def calibrate(self, paper_length_mm: float) -> None:
        self._send(proto.cmd_calib_arm(paper_length_mm))

    def sw_trigger(self) -> None:
        self._send(proto.cmd_sw_trigger())

    def reset_sheet_count(self) -> None:
        self.status.sheet_count = 0
        self.status_changed.emit(self.status)

    # ---- inbound (firmware -> UI) ------------------------------------------
    def _on_event(self, ev: dict[str, Any]) -> None:
        self.log_appended.emit(ev)
        kind = ev.get("event", "")
        if kind == proto.EVT_STATUS:
            s = self.status
            s.active      = bool(ev.get("active", s.active))
            s.fault       = bool(ev.get("fault", s.fault))
            s.speed_mm_s  = float(ev.get("speed_mm_s", s.speed_mm_s))
            s.sheet_count = int(ev.get("sheet_count", s.sheet_count))
            s.queue_depth = int(ev.get("queue_depth", s.queue_depth))
            self.status_changed.emit(s)
        elif kind == proto.EVT_ERROR:
            self.error_received.emit(ev.get("cmd", ""), ev.get("reason", ""))
        elif kind == proto.EVT_CALIB_RESULT:
            ppm = ev.get("pulses_per_mm")
            if isinstance(ppm, (int, float)):
                self.config.pulses_per_mm = float(ppm)
                self.config_changed.emit(self.config)

    def _on_link_conn(self, ok: bool, reason: str) -> None:
        self.connection_changed.emit(ok, reason)

    def _ping(self) -> None:
        if self.link.connected:
            self.link.send(proto.cmd_ping())
