"""Save / load complete operator profiles (config + 4 patterns) as JSON."""
from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path
from typing import Any

from . import protocol as proto
from .state import AppState, GunPattern, RuntimeConfig

SCHEMA_VERSION = 1


def to_dict(state: AppState) -> dict[str, Any]:
    return {
        "version": SCHEMA_VERSION,
        "config":  asdict(state.config),
        "patterns": [
            {
                "type": proto.PatternType(gp.type).value,
                "on_timeout_ms": gp.on_timeout_ms,
                "elements": [
                    {"start": e.start_mm, "end": e.end_mm,
                     "spacing": e.spacing_mm}
                    for e in gp.elements
                ],
            }
            for gp in state.patterns
        ],
    }


def save_to(path: str | Path, state: AppState) -> None:
    Path(path).write_text(
        json.dumps(to_dict(state), ensure_ascii=False, indent=2),
        encoding="utf-8")


def load_from(path: str | Path, state: AppState) -> None:
    obj = json.loads(Path(path).read_text(encoding="utf-8"))
    apply_dict(obj, state)


def apply_dict(obj: dict[str, Any], state: AppState) -> None:
    """Load a profile dict into `state` and push it live to the firmware."""
    if not isinstance(obj, dict):
        raise ValueError("bad profile file")

    # Config
    cfg_in = obj.get("config", {})
    new_cfg = RuntimeConfig(**{
        k: v for k, v in cfg_in.items()
        if k in RuntimeConfig.__dataclass_fields__
    })
    state.config = new_cfg
    state.config_changed.emit(state.config)
    # Push to firmware (live).
    state.push_config(**{k: getattr(new_cfg, k)
                         for k in RuntimeConfig.__dataclass_fields__})

    # Patterns
    pats_in = obj.get("patterns", [])
    for i in range(proto.NUM_GUNS):
        gp = state.patterns[i]
        gp.type = proto.PatternType.NONE
        gp.elements = []
        gp.on_timeout_ms = 1.2
        if i < len(pats_in):
            raw = pats_in[i] or {}
            try:
                gp.type = proto.PatternType(raw.get("type", "none"))
            except ValueError:
                gp.type = proto.PatternType.NONE
            # Accept legacy `hold_time_ms` from older profile files.
            gp.on_timeout_ms = float(
                raw.get("on_timeout_ms",
                        raw.get("hold_time_ms", 1.2)))
            for el in raw.get("elements", []):
                gp.elements.append(proto.PatternElement(
                    start_mm=float(el.get("start", 0.0)),
                    end_mm=float(el.get("end", 0.0)),
                    spacing_mm=float(el.get("spacing", 0.0)),
                ))
        state.pattern_changed.emit(i, gp)
        # Push to firmware (live).
        state.push_pattern(i)
