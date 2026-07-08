"""NDJSON protocol — command builders and event constants.

Wire format: one JSON object per line, '\n' terminated, UTF-8.
Mirror of `src/comms/UartJson.cpp` command set. Guns are 1-based on the wire
(1..4), with 0 = all for `test_open`.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any


NUM_GUNS = 4
MAX_ELEMENTS_PER_GUN = 64


class PatternType(str, Enum):
    NONE  = "none"
    LINES = "lines"
    DOTS  = "dots"


@dataclass
class PatternElement:
    start_mm: float
    end_mm: float
    spacing_mm: float = 0.0  # 0 -> line, >0 -> dots interval


# -----------------------------------------------------------------------------
# Command builders — return a JSON-serialisable dict.
# -----------------------------------------------------------------------------

def cmd_ping() -> dict[str, Any]:
    return {"cmd": "ping"}


def cmd_set_active(active: bool) -> dict[str, Any]:
    return {"cmd": "set_active", "active": bool(active)}


def cmd_set_config(**fields: float) -> dict[str, Any]:
    """Pass any subset of: pulses_per_mm, min_speed_mm_s, photocell_offset_mm,
    debounce_ms, pick_current_a, hold_current_a."""
    allowed = {
        "pulses_per_mm", "min_speed_mm_s", "photocell_offset_mm",
        "debounce_ms", "pick_current_a", "hold_current_a",
    }
    bad = set(fields) - allowed
    if bad:
        raise ValueError(f"unknown set_config fields: {bad}")
    return {"cmd": "set_config", **fields}


def cmd_set_pattern(gun_1based: int, ptype: PatternType,
                    elements: list[PatternElement],
                    on_timeout_ms: float | None = None) -> dict[str, Any]:
    """Build a `set_pattern` command. `on_timeout_ms` is the per-gun total
    Peak+Hold budget for one fire cycle (measured from the start of Phase 1).
    In Dots mode it equals the droplet on-time. In Lines mode the value is
    used only as the long safety ceiling; line termination is encoder-
    position driven."""
    if not 1 <= gun_1based <= NUM_GUNS:
        raise ValueError(f"gun out of range: {gun_1based}")
    # Qt stores str-Enum userData as a plain str, so coerce back defensively.
    ptype = PatternType(ptype)
    if len(elements) > MAX_ELEMENTS_PER_GUN:
        raise ValueError("too many elements")
    elems: list[dict[str, float]] = []
    for e in elements:
        d: dict[str, float] = {"start": e.start_mm, "end": e.end_mm}
        if ptype == PatternType.DOTS:
            d["spacing"] = e.spacing_mm
        elems.append(d)
    payload: dict[str, Any] = {"cmd": "set_pattern", "gun": gun_1based,
                               "type": ptype.value, "elements": elems}
    if on_timeout_ms is not None:
        payload["on_timeout_ms"] = float(on_timeout_ms)
    return payload


def cmd_test_open(gun_1based: int, timeout_ms: int = 1000) -> dict[str, Any]:
    if not 0 <= gun_1based <= NUM_GUNS:
        raise ValueError(f"gun out of range: {gun_1based}")
    if not 1 <= timeout_ms <= 5000:
        raise ValueError("timeout_ms must be 1..5000")
    return {"cmd": "test_open", "gun": gun_1based, "timeout_ms": timeout_ms}


def cmd_test_close(gun_1based: int = 0) -> dict[str, Any]:
    return {"cmd": "test_close", "gun": gun_1based}


def cmd_calib_arm(paper_length_mm: float) -> dict[str, Any]:
    return {"cmd": "calib_arm", "paper_length_mm": float(paper_length_mm)}


def cmd_sw_trigger() -> dict[str, Any]:
    return {"cmd": "sw_trigger"}


# -----------------------------------------------------------------------------
# Event names (received from firmware).
# -----------------------------------------------------------------------------

EVT_READY            = "ready"
EVT_STATUS           = "status"
EVT_ACK              = "ack"
EVT_ERROR            = "error"
EVT_PATTERN_EVENT    = "pattern_event"
EVT_WATCHDOG_TIMEOUT = "watchdog_timeout"
EVT_CALIB_RESULT     = "calib_result"
