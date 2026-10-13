"""Admin mode: changing settings needs the password.

Only a guard against accidental changes on the shop floor -- the password is
shown in the prompt and is not a security boundary.  Programs, start/stop and
gun tests stay open; admin mode ends after ADMIN_MINUTES or when the operator
leaves it.
"""
from __future__ import annotations

import logging
import time

from PySide6.QtCore import QObject, QTimer, Signal

PASSWORD = "1234"
ADMIN_MINUTES = 10

LOGGER = logging.getLogger("admin")


class AdminMode(QObject):
    changed = Signal(bool)       # True = admin mode on

    def __init__(self) -> None:
        super().__init__()
        self._until = 0.0
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self.lock)

    @property
    def active(self) -> bool:
        return self._until > time.monotonic()

    def remaining_s(self) -> int:
        return max(0, int(self._until - time.monotonic() + 0.999))

    def unlock(self, password: str) -> bool:
        if password != PASSWORD:
            LOGGER.warning("admin: wrong password")
            return False
        self._until = time.monotonic() + ADMIN_MINUTES * 60
        self._timer.start(ADMIN_MINUTES * 60 * 1000)
        LOGGER.info("admin mode on (%d min)", ADMIN_MINUTES)
        self.changed.emit(True)
        return True

    def lock(self) -> None:
        was = self._until > 0.0
        self._until = 0.0
        self._timer.stop()
        if was:
            LOGGER.info("admin mode off")
            self.changed.emit(False)
