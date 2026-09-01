"""Program selector / manager bar.

The controller's SPIFFS program store is the single source of truth.  This bar
lists programs from the controller, lets the operator switch to one, and
save/rename/delete/create programs via serial commands.
"""
from __future__ import annotations

from typing import Callable

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QComboBox, QHBoxLayout, QInputDialog, QLabel,
                               QMessageBox, QPushButton, QWidget)

import logging
from app import protocol as proto
from app.state import AppState


LOGGER = logging.getLogger("program_bar")


class ProgramBar(QWidget):
    def __init__(self, state: AppState, refresh_cb: Callable[[], None]) -> None:
        super().__init__()
        self.state = state
        self._refresh = refresh_cb
        self._loading = False

        self.combo = QComboBox()
        self.combo.setMinimumWidth(220)
        self.combo.currentIndexChanged.connect(self._on_combo_changed)

        btn_save    = QPushButton("שמור")
        btn_save_as = QPushButton("שמירה בשם…")
        btn_new     = QPushButton("חדשה")
        btn_del     = QPushButton("מחק")
        btn_save.clicked.connect(self._on_save)
        btn_save_as.clicked.connect(self._on_save_as)
        btn_new.clicked.connect(self._on_new)
        btn_del.clicked.connect(self._on_delete)

        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 6, 10, 6)
        lay.addWidget(QLabel("תוכנית:"))
        lay.addWidget(self.combo)
        lay.addWidget(btn_save)
        lay.addWidget(btn_save_as)
        lay.addWidget(btn_new)
        lay.addStretch(1)
        lay.addWidget(btn_del)

        state.programs_changed.connect(self._on_programs_changed)

    # ---- combo helpers -------------------------------------------------------
    def _id_for_name(self, name: str) -> int:
        for p in self.state.programs:
            if p.get("name") == name:
                return int(p.get("id", 0))
        return 0

    def _on_programs_changed(self, programs: list) -> None:
        LOGGER.info("programs_changed: %s", programs)
        self._loading = True
        current_id = self._current_id()
        self.combo.blockSignals(True)
        self.combo.clear()
        for p in programs:
            name = p.get("name", "")
            pid = p.get("id", 0)
            self.combo.addItem(name, pid)
        # Prefer the controller's active program; fall back to current selection.
        select_id = self.state.active_program_id or current_id
        for i in range(self.combo.count()):
            if self.combo.itemData(i) == select_id:
                self.combo.setCurrentIndex(i)
                break
        self.combo.blockSignals(False)
        self._loading = False

    def _current_id(self) -> int:
        return int(self.combo.currentData() or 0)

    # ---- handlers ------------------------------------------------------------
    def _on_combo_changed(self, _idx: int) -> None:
        if self._loading:
            return
        pid = self._current_id()
        LOGGER.info("combo changed -> load_program id=%s", pid)
        if pid:
            self.state.load_program(pid)

    def _on_save(self) -> None:
        pid = self._current_id()
        name = self.combo.currentText()
        if not pid or not name:
            return
        self.state.save_program(pid, name)

    def _on_save_as(self) -> None:
        name, ok = QInputDialog.getText(self, "שמירה בשם", "שם התוכנית:")
        name = name.strip()
        if not ok or not name:
            return
        pid = self._id_for_name(name)
        if pid and QMessageBox.question(
                self, "קיים", f"'{name}' קיימת. להחליף?") != \
                QMessageBox.StandardButton.Yes:
            return
        self.state.save_program(pid, name)

    def _on_new(self) -> None:
        name, ok = QInputDialog.getText(self, "תוכנית חדשה", "שם התוכנית:")
        name = name.strip()
        if not ok or not name:
            return
        if self._id_for_name(name):
            QMessageBox.warning(self, "קיים", f"'{name}' כבר קיימת.")
            return
        # Clear patterns locally and push them to the controller before saving.
        LOGGER.info("creating new program '%s'", name)
        for i in range(proto.NUM_GUNS):
            gp = self.state.patterns[i]
            gp.type = proto.PatternType.NONE
            gp.elements = []
            gp.on_timeout_ms = 1.2
            self.state.push_pattern(i)
        self.state.save_program(0, name)

    def _on_delete(self) -> None:
        pid = self._current_id()
        name = self.combo.currentText()
        if not pid:
            return
        if QMessageBox.question(self, "מחיקה", f"למחוק את '{name}'?") != \
                QMessageBox.StandardButton.Yes:
            return
        self.state.delete_program(pid)
