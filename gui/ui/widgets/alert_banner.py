"""Operator alert banner shown under the header (safety trips / stops)."""
from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton


class AlertBanner(QFrame):
    """Hidden until `show_alert`.  level "warn" (amber) or "stop" (red).
    A timeout of 0 keeps the banner until the operator closes it or
    `clear("stop")` is called."""

    def __init__(self) -> None:
        super().__init__()
        self.setObjectName("AlertBanner")
        self._level = ""

        self.icon = QLabel()
        self.icon.setObjectName("AlertIcon")
        self.text = QLabel()
        self.text.setObjectName("AlertText")
        self.text.setWordWrap(True)
        self.text.setTextInteractionFlags(Qt.TextSelectableByMouse)
        btn_close = QPushButton("✕")
        btn_close.setObjectName("AlertClose")
        btn_close.setToolTip("סגור הודעה")
        btn_close.clicked.connect(self.hide)

        lay = QHBoxLayout(self)
        lay.setContentsMargins(14, 10, 14, 10)
        lay.setSpacing(12)
        lay.addWidget(self.icon, 0, Qt.AlignTop)
        lay.addWidget(self.text, 1)
        lay.addWidget(btn_close, 0, Qt.AlignTop)

        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self.hide)
        self.hide()

    @property
    def level(self) -> str:
        return self._level if self.isVisible() else ""

    def show_alert(self, text: str, level: str, timeout_ms: int = 0) -> None:
        # A machine-stop message is not replaced by a later warning.
        if self.level == "stop" and level != "stop":
            return
        self._level = level
        self.setProperty("level", level)
        self.style().unpolish(self)
        self.style().polish(self)
        self.icon.setText("⛔" if level == "stop" else "⚠")
        self.text.setText(text)
        self._timer.stop()
        if timeout_ms:
            self._timer.start(timeout_ms)
        self.show()

    def clear(self, level: str) -> None:
        if self.level == level:
            self.hide()
