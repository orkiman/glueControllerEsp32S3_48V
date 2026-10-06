"""Compact header run controls: start/stop toggle, state, speed, sheet count."""
from __future__ import annotations

from PySide6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QPushButton,
                               QWidget)

from app.state import AppState, LiveStatus


class RunPanel(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state

        self.btn_action = QPushButton("▶  הפעל")
        self.btn_action.setObjectName("PrimaryButton")
        self.btn_action.setMinimumWidth(120)
        self.btn_action.clicked.connect(self._toggle_active)

        self.lbl_state  = self._value("—", "○  לא פעיל")
        self.lbl_speed  = self._value("0 mm/s", "0000 mm/s")
        self.lbl_sheets = self._value("0", "000000")

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
        self.btn_action.setText("■  עצור" if active else "▶  הפעל")
        name = "DangerButton" if active else "PrimaryButton"
        if self.btn_action.objectName() != name:
            self.btn_action.setObjectName(name)
            self.btn_action.style().unpolish(self.btn_action)
            self.btn_action.style().polish(self.btn_action)

    def _on_status(self, s: LiveStatus) -> None:
        if not self.state.link.connected:
            return
        if s.fault:
            self.lbl_state.setText("⚠  תקלה")
            self.lbl_state.setStyleSheet("color:#ef4444;")
        elif s.active:
            self.lbl_state.setText("●  פעיל")
            self.lbl_state.setStyleSheet("color:#22c55e;")
        else:
            self.lbl_state.setText("○  לא פעיל")
            self.lbl_state.setStyleSheet("color:#ef4444;")
        self._set_button(s.active)
        self.lbl_speed.setText(f"{s.speed_mm_s:.0f} mm/s")
        self.lbl_sheets.setText(str(s.sheet_count))

    def _on_conn(self, ok: bool, _reason: str) -> None:
        self.btn_action.setEnabled(ok)
        if not ok:
            # Values are stale without a link -- don't show a misleading state.
            self.lbl_state.setText("—")
            self.lbl_state.setStyleSheet("")
