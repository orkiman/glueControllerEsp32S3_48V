"""Operate screen: large run/stop, live speed, sheet counter, fault indicator."""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QGridLayout, QGroupBox, QHBoxLayout, QLabel,
                               QPushButton, QVBoxLayout, QWidget)

from app.state import AppState, LiveStatus


class OperateScreen(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state

        # ---- big actions ----
        self.btn_action = QPushButton("הפעל")
        self.btn_action.setObjectName("PrimaryButton")
        self.btn_action.setMinimumHeight(72)
        self.btn_action.clicked.connect(self._toggle_active)

        actions = QHBoxLayout()
        actions.addWidget(self.btn_action, 1)

        # ---- live stats ----
        self.lbl_state = self._stat("מצב", "—")
        self.lbl_speed = self._stat("מהירות קו", "0 mm/s")
        self.lbl_sheets = self._stat("ספירת דפים", "0")

        btn_reset = QPushButton("אפס ספירה")
        btn_reset.clicked.connect(state.reset_sheet_count)

        stats_box = QGroupBox("נתונים בזמן אמת")
        grid = QGridLayout(stats_box)
        grid.addWidget(self.lbl_state[0],  0, 0, alignment=Qt.AlignCenter)
        grid.addWidget(self.lbl_speed[0],  0, 1, alignment=Qt.AlignCenter)
        grid.addWidget(self.lbl_sheets[0], 0, 2, alignment=Qt.AlignCenter)
        grid.addWidget(self.lbl_state[1],  1, 0, alignment=Qt.AlignCenter)
        grid.addWidget(self.lbl_speed[1],  1, 1, alignment=Qt.AlignCenter)
        grid.addWidget(self.lbl_sheets[1], 1, 2, alignment=Qt.AlignCenter)
        grid.addWidget(btn_reset,          2, 2, alignment=Qt.AlignCenter)

        # ---- compose ----
        root = QVBoxLayout(self)
        root.addLayout(actions)
        root.addWidget(stats_box)
        root.addStretch(1)

        state.status_changed.connect(self._on_status)
        state.error_received.connect(self._on_error)
        self._on_status(state.status)

    @staticmethod
    def _stat(caption: str, value: str) -> tuple[QLabel, QLabel]:
        v = QLabel(value); v.setObjectName("StatLabel")
        c = QLabel(caption); c.setObjectName("StatCaption")
        return v, c

    def _toggle_active(self) -> None:
        self.state.set_active(not self.state.status.active)

    def _on_status(self, s: LiveStatus) -> None:
        if s.fault:
            self.lbl_state[0].setText("⚠ תקלה")
            self.lbl_state[0].setStyleSheet("color:#ef4444;")
        elif s.active:
            self.lbl_state[0].setText("● פעיל")
            self.lbl_state[0].setStyleSheet("color:#22c55e;")
            self.btn_action.setText("עצור")
            self.btn_action.setObjectName("DangerButton")
            self.btn_action.setStyleSheet("")
        else:
            self.lbl_state[0].setText("○ לא פעיל")
            self.lbl_state[0].setStyleSheet("color:#ef4444;")
            self.btn_action.setText("הפעל")
            self.btn_action.setObjectName("PrimaryButton")
            self.btn_action.setStyleSheet("")
        self.lbl_speed[0].setText(f"{s.speed_mm_s:.0f} mm/s")
        self.lbl_sheets[0].setText(str(s.sheet_count))

    def _on_error(self, cmd: str, reason: str) -> None:
        # Lightweight surfacing — full log lives on Admin screen.
        pass
