"""pyserial-based NDJSON link.

A QThread owns the serial port and emits `event_received` for each line.
TX goes through a thread-safe queue; the worker drains it between reads.
"""
from __future__ import annotations

import json
import queue
from typing import Any

import serial
from PySide6.QtCore import QThread, Signal

from .link_base import LinkBase


def list_ports() -> list[tuple[str, str]]:
    """Return [(device, description), ...] for every detected COM port."""
    from serial.tools import list_ports as _lp
    return [(p.device, p.description) for p in _lp.comports()]


class _RxTxThread(QThread):
    line_received = Signal(str)
    failed        = Signal(str)

    def __init__(self, port: str, baud: int = 115200) -> None:
        super().__init__()
        self._port = port
        self._baud = baud
        self._tx_q: queue.Queue[bytes] = queue.Queue()
        self._stop = False

    def send_bytes(self, data: bytes) -> None:
        self._tx_q.put(data)

    def request_stop(self) -> None:
        self._stop = True

    def run(self) -> None:  # noqa: D401
        try:
            ser = serial.Serial(self._port, self._baud, timeout=0.05)
        except Exception as exc:  # noqa: BLE001
            self.failed.emit(str(exc))
            return

        buf = bytearray()
        try:
            while not self._stop:
                # TX drain (non-blocking)
                try:
                    while True:
                        ser.write(self._tx_q.get_nowait())
                except queue.Empty:
                    pass

                chunk = ser.read(256)
                if chunk:
                    buf.extend(chunk)
                    while b"\n" in buf:
                        line, _, rest = buf.partition(b"\n")
                        buf = bytearray(rest)
                        s = line.decode("utf-8", errors="replace").strip()
                        if s:
                            self.line_received.emit(s)
        except Exception as exc:  # noqa: BLE001
            self.failed.emit(str(exc))
        finally:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass


class SerialLink(LinkBase):
    def __init__(self) -> None:
        super().__init__()
        self._thread: _RxTxThread | None = None

    # ---- API ----------------------------------------------------------------
    def open(self, port: str, baud: int = 115200) -> None:  # noqa: A003
        self.close()
        t = _RxTxThread(port, baud)
        t.line_received.connect(self._on_line)
        t.failed.connect(self._on_failed)
        t.finished.connect(lambda: self._set_connected(False, "closed"))
        self._thread = t
        t.start()
        # Optimistic; if it fails, _on_failed will flip us back.
        self._set_connected(True, port)

    def close(self) -> None:
        if self._thread is not None:
            self._thread.request_stop()
            self._thread.wait(1000)
            self._thread = None
        self._set_connected(False, "closed")

    def send(self, payload: dict[str, Any]) -> None:
        if self._thread is None:
            return
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
        self._thread.send_bytes(data)

    # ---- internals ----------------------------------------------------------
    def _on_line(self, line: str) -> None:
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            # Not JSON: almost always firmware boot banner or an ESP32 panic
            # dump (Guru Meditation / backtrace).  Surface it as a `raw` event
            # so crashes are visible in the log instead of vanishing.
            self.event_received.emit({"event": "raw", "text": line})
            return
        if isinstance(obj, dict):
            self.event_received.emit(obj)

    def _on_failed(self, reason: str) -> None:
        self._set_connected(False, reason)
