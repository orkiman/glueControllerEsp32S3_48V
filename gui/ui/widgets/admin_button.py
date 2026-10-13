"""Button that enters / leaves admin mode and shows the time left."""
from __future__ import annotations

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QInputDialog, QLineEdit, QMessageBox, QPushButton

from app.admin import ADMIN_MINUTES, PASSWORD, AdminMode


class AdminButton(QPushButton):
    def __init__(self, admin: AdminMode) -> None:
        super().__init__()
        self.admin = admin
        self.setObjectName("AdminButton")
        self.clicked.connect(self._on_click)
        # Countdown refresh while admin mode is on.
        self._tick = QTimer(self)
        self._tick.setInterval(1000)
        self._tick.timeout.connect(self._refresh)
        admin.changed.connect(self._on_changed)
        self._on_changed(admin.active)

    def _on_click(self) -> None:
        if self.admin.active:
            self.admin.lock()                      # leave admin mode now
            return
        pw, ok = QInputDialog.getText(
            self, "מצב מנהל", f"הקש סיסמה ({PASSWORD}):", QLineEdit.Password)
        if not ok:
            return
        if not self.admin.unlock(pw):
            QMessageBox.warning(self, "מצב מנהל", "סיסמה שגויה")

    def _on_changed(self, on: bool) -> None:
        if on:
            self._tick.start()
        else:
            self._tick.stop()
        self._refresh()

    def _refresh(self) -> None:
        if self.admin.active:
            m, s = divmod(self.admin.remaining_s(), 60)
            self.setText(f"🔓  מצב מנהל {m}:{s:02d} — יציאה")
            self.setToolTip("לחיצה יוצאת ממצב מנהל מיד")
        else:
            self.setText("🔒  כניסת מנהל")
            self.setToolTip(
                f"שינוי הגדרות דורש סיסמה. מצב מנהל נשאר פעיל {ADMIN_MINUTES} דקות.")
