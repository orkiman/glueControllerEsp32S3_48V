"""Compact header run controls: start/stop toggle, state, speed, sheet count."""
from __future__ import annotations

from PySide6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QPushButton,
                               QWidget)

from app.state import AppState, LiveStatus

# Machine state colours: lots of green while running, lots of red while stopped.
STATE_COLORS = {
    "active": "#16a34a",
    "stopped": "#dc2626",
    "fault": "#dc2626",
    "calib": "#2563eb",
    "none": "#2d333b",       # disconnected / unknown
}


def state_key(s: LiveStatus) -> str:
    if s.fault:
        return "fault"
    if s.calibrating:
        return "calib"
    return "active" if s.active else "stopped"


class StateStrip(QFrame):
    """Full-width coloured strip under the header: green running, red stopped."""

    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state
        self.setFixedHeight(10)
        self._paint("none")
        state.status_changed.connect(
            lambda s: self._paint(state_key(s) if state.link.connected else "none"))
        state.connection_changed.connect(
            lambda ok, _r: self._paint(state_key(state.status) if ok else "none"))

    def _paint(self, key: str) -> None:
        self.setStyleSheet(f"background-color:{STATE_COLORS[key]}; border:none;")


class RunPanel(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state

        self.btn_action = QPushButton("▶  הפעל")
        self.btn_action.setObjectName("PrimaryButton")
        self.btn_action.setMinimumWidth(120)
        self.btn_action.clicked.connect(self._toggle_active)

        self.lbl_state  = self._value("—", "■  לא פעיל")
        self.lbl_state.setMinimumWidth(self.lbl_state.minimumWidth() + 40)
        self.lbl_speed  = self._value("0 mm/s", "0000 mm/s")
        self.lbl_sheets = self._value("0", "000000")
        # Photocell (INPUT1): lit while it sees a sheet; tooltip = 24V / 0V.
        self.lbl_eye    = self._value("—", "○  אין דף")

        btn_reset = QPushButton("↺")
        btn_reset.setObjectName("IconButton")
        btn_reset.setToolTip("אפס ספירת דפים")
        btn_reset.clicked.connect(state.reset_sheet_count)

        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(8)
        lay.addWidget(self.btn_action)
        lay.addSpacing(6)
        lay.addWidget(self.lbl_state)
        lay.addWidget(self._separator())
        lay.addWidget(self._caption("מהירות"))
        lay.addWidget(self.lbl_speed)
        lay.addWidget(self._separator())
        lay.addWidget(self._caption("דפים"))
        lay.addWidget(self.lbl_sheets)
        lay.addWidget(btn_reset)
        lay.addWidget(self._separator())
        lay.addWidget(self._caption("עין"))
        lay.addWidget(self.lbl_eye)

        state.status_changed.connect(self._on_status)
        state.connection_changed.connect(self._on_conn)
        self._on_conn(state.link.connected, "")

    # ---- builders -----------------------------------------------------------
    @staticmethod
    def _value(text: str, widest: str) -> QLabel:
        lbl = QLabel(text)
        lbl.setObjectName("HeaderValue")
        # Reserve room for the widest expected text so the header doesn't
        # jitter as numbers change.
        lbl.ensurePolished()
        lbl.setMinimumWidth(lbl.fontMetrics().horizontalAdvance(widest))
        return lbl

    @staticmethod
    def _caption(text: str) -> QLabel:
        lbl = QLabel(text)
        lbl.setObjectName("HeaderCaption")
        return lbl

    @staticmethod
    def _separator() -> QFrame:
        sep = QFrame()
        sep.setObjectName("HeaderSeparator")
        sep.setFrameShape(QFrame.VLine)
        sep.setFixedWidth(1)
        return sep

    # ---- behaviour ----------------------------------------------------------
    def _toggle_active(self) -> None:
        self.state.set_active(not self.state.status.active)

    def _set_button(self, active: bool) -> None:
        # Stays blue in both states; the state pill and strip carry the colour.
        self.btn_action.setText("■  עצור" if active else "▶  הפעל")

    def _on_status(self, s: LiveStatus) -> None:
        if not self.state.link.connected:
            return
        key = state_key(s)
        self.lbl_state.setText({"fault": "⚠  תקלה", "calib": "◎  קליברציה",
                                "active": "●  פעיל", "stopped": "■  לא פעיל"}[key])
        self.lbl_state.setStyleSheet(
            f"background-color:{STATE_COLORS[key]}; color:#ffffff;"
            " border-radius:6px; padding:2px 14px;")
        self._set_button(s.active)
        self.lbl_speed.setText(f"{s.speed_mm_s:.0f} mm/s")
        self.lbl_sheets.setText(str(s.sheet_count))
        self.lbl_eye.setText("●  דף" if s.eye else "○  אין דף")
        self.lbl_eye.setStyleSheet("color:#22c55e;" if s.eye else "color:#9ca6b3;")
        self.lbl_eye.setToolTip(f"INPUT1: {'24V' if s.in1_24v else '0V'}")

    def _on_conn(self, ok: bool, _reason: str) -> None:
        self.btn_action.setEnabled(ok)
        if not ok:
            # Values are stale without a link -- don't show a misleading state.
            for lbl in (self.lbl_state, self.lbl_eye):
                lbl.setText("—")
                lbl.setStyleSheet("")
