"""Top bar: COM picker + connect/disconnect + status LED."""
from __future__ import annotations

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (QComboBox, QHBoxLayout, QLabel, QPushButton,
                               QWidget)

from app.serial_link import SerialLink, list_ports
from app.state import AppState


SETTINGS = QSettings("orkiman", "glue_controller")
LAST_PORT_KEY = "last_port"


class StatusLed(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFixedSize(18, 18)
        self._on = False

    def setOn(self, on: bool) -> None:  # noqa: N802 — Qt-style
        if on != self._on:
            self._on = on
            self.update()

    def paintEvent(self, _ev):  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        color = QColor("#22c55e") if self._on else QColor("#ef4444")
        p.setBrush(color)
        p.setPen(Qt.NoPen)
        p.drawEllipse(2, 2, 14, 14)


class ConnectionBar(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state
        self._auto_connect = False

        self.combo = QComboBox()
        self.combo.setMinimumWidth(260)
        self.combo.currentIndexChanged.connect(self._on_port_changed)
        self.btn   = QPushButton("התחבר")
        self.btn.clicked.connect(self._toggle)
        self.refresh = QPushButton("רענן")
        self.refresh.clicked.connect(self._populate)

        self.led = StatusLed()
        self.status_label = QLabel("מנותק")

        lay = QHBoxLayout(self)
        lay.setContentsMargins(12, 8, 12, 8)
        lay.addWidget(QLabel("יציאה:"))
        lay.addWidget(self.combo)
        lay.addWidget(self.refresh)
        lay.addWidget(self.btn)
        lay.addStretch(1)
        lay.addWidget(self.led)
        lay.addWidget(self.status_label)

        self._populate()
        state.connection_changed.connect(self._on_conn)

        # Disable connect controls if we're running on a mock link, and try
        # to auto-connect to the last used port on startup.
        if isinstance(state.link, SerialLink):
            self._try_auto_connect()
        else:
            self.combo.setEnabled(False)
            self.btn.setEnabled(False)
            self.refresh.setEnabled(False)

        # Make sure the bar has the correct color from the moment the UI is
        # shown (before any connection_changed signal arrives).
        self._apply_conn_style(self.state.link.connected)

    def _populate(self) -> None:
        self.combo.blockSignals(True)
        self.combo.clear()
        last = SETTINGS.value(LAST_PORT_KEY, "")
        select_idx = -1
        for i, (dev, desc) in enumerate(list_ports()):
            self.combo.addItem(f"{dev} — {desc}", dev)
            if dev == last:
                select_idx = i
        if self.combo.count() == 0:
            self.combo.addItem("(לא נמצאו יציאות)", None)
        elif select_idx >= 0:
            self.combo.setCurrentIndex(select_idx)
        self.combo.blockSignals(False)

    def _try_auto_connect(self) -> None:
        """On startup, try to connect to the last used COM port.
        If the port is still unavailable, leave the UI disconnected."""
        port = self.combo.currentData()
        if port and not self.state.link.connected:
            self._auto_connect = True
            self.state.link.open(port)

    def _set_active_safe(self) -> None:
        """Send set_active(true) only if the link is still connected."""
        if self.state.link.connected:
            self.state.set_active(True)

    def _on_port_changed(self, _idx: int) -> None:
        if not isinstance(self.state.link, SerialLink):
            return
        port = self.combo.currentData()
        if self.state.link.connected:
            self.state.link.close()
        if port:
            self.state.link.open(port)

    def _toggle(self) -> None:
        link = self.state.link
        if link.connected:
            link.close()
        else:
            port = self.combo.currentData()
            if port:
                link.open(port)

    def _apply_conn_style(self, connected: bool) -> None:
        """Color the entire connection bar to make the connection state obvious.
        Red when disconnected, back to the normal dark theme when connected."""
        if connected:
            self.setStyleSheet("")
            return

        # Disconnected state: bright red background everywhere in the bar.
        self.setStyleSheet("""
            ConnectionBar {
                background-color: #ef4444;
                color: #0c0f13;
            }
            ConnectionBar QLabel {
                background-color: #ef4444;
                color: #f8fafc;
            }
            ConnectionBar QComboBox {
                background-color: #ef4444;
                color: #f8fafc;
                border: 1px solid #0c0f13;
                border-radius: 4px;
                padding: 6px 10px;
                selection-background-color: #11141a;
                selection-color: #f8fafc;
            }
            ConnectionBar QComboBox::drop-down {
                border: 0;
                width: 24px;
            }
            ConnectionBar QComboBox::down-arrow {
                border-top: 6px solid #f8fafc;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
            }
            ConnectionBar QComboBox QAbstractItemView {
                background-color: #ef4444;
                color: #f8fafc;
                selection-background-color: #11141a;
                selection-color: #f8fafc;
            }
            ConnectionBar QPushButton {
                background-color: #ef4444;
                color: #f8fafc;
                border: 1px solid #0c0f13;
                border-radius: 4px;
                padding: 8px 16px;
                min-height: 22px;
            }
            ConnectionBar QPushButton:hover {
                background-color: #0c0f13;
                color: #f8fafc;
            }
            ConnectionBar QPushButton:pressed {
                background-color: #11141a;
                color: #f8fafc;
            }
            ConnectionBar QPushButton:disabled {
                background-color: #23272d;
                color: #6b727c;
            }
        """)

    def _on_conn(self, ok: bool, reason: str) -> None:
        self.led.setOn(ok)
        self._apply_conn_style(ok)
        if ok:
            self.status_label.setText(f"מחובר ({reason})")
            self.btn.setText("נתק")
            port = self.combo.currentData()
            if port:
                SETTINGS.setValue(LAST_PORT_KEY, port)
            if self._auto_connect:
                self._auto_connect = False
                # Defer to the next event-loop iteration so this is sent
                # after AppState._on_link_conn runs push_full_state (which
                # ends with a set_active(False)).
                QTimer.singleShot(0, self._set_active_safe)
        else:
            self.status_label.setText("מנותק" if not reason else f"מנותק — {reason}")
            self.btn.setText("התחבר")
            self._auto_connect = False
