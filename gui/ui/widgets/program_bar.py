"""Program selector / manager bar.

Lets the operator pick a saved program from a dropdown, save the current
one (overwrite or save-as), create a new empty one, or delete one. The
current program is auto-saved (debounced) on every edit so the last program
is always restored on the next launch.
"""
from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QComboBox, QHBoxLayout, QInputDialog, QLabel,
                               QMessageBox, QPushButton, QWidget)

from app import protocol as proto
from app.programs import DEFAULT_NAME, ProgramStore
from app.state import AppState


class ProgramBar(QWidget):
    def __init__(self, state: AppState, store: ProgramStore,
                 refresh_cb: Callable[[], None]) -> None:
        super().__init__()
        self.state   = state
        self.store   = store
        self._refresh = refresh_cb
        self._loading = False   # guards programmatic state changes

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

        # Debounced auto-save so the active program always persists.
        self._autosave = QTimer(self)
        self._autosave.setInterval(600)
        self._autosave.setSingleShot(True)
        self._autosave.timeout.connect(self._do_autosave)
        state.pattern_changed.connect(lambda *_: self._schedule_autosave())
        state.config_changed.connect(lambda *_: self._schedule_autosave())

        self._init_programs()

    # ---- startup -----------------------------------------------------------
    def _init_programs(self) -> None:
        """Restore the last program, or seed a default empty one."""
        self._loading = True
        names = self.store.names()
        if not names:
            self.store.save_program(DEFAULT_NAME, self.state)
            names = self.store.names()
        target = self.store.last or names[0]
        self.store.apply(target, self.state)
        self._reload_combo(select=target)
        self._loading = False
        self._refresh()

    # ---- combo helpers -----------------------------------------------------
    def _reload_combo(self, select: str | None = None) -> None:
        self.combo.blockSignals(True)
        self.combo.clear()
        self.combo.addItems(self.store.names())
        if select is not None:
            idx = self.combo.findText(select)
            if idx >= 0:
                self.combo.setCurrentIndex(idx)
        self.combo.blockSignals(False)

    def current_name(self) -> str:
        return self.combo.currentText() or DEFAULT_NAME

    # ---- handlers ----------------------------------------------------------
    def _on_combo_changed(self, _idx: int) -> None:
        if self._loading:
            return
        name = self.combo.currentText()
        if not name:
            return
        self._loading = True
        self.store.apply(name, self.state)
        self._loading = False
        self._refresh()

    def _on_save(self) -> None:
        self.store.save_program(self.current_name(), self.state)

    def _on_save_as(self) -> None:
        name, ok = QInputDialog.getText(self, "שמירה בשם", "שם התוכנית:")
        name = name.strip()
        if not ok or not name:
            return
        if self.store.exists(name) and QMessageBox.question(
                self, "קיים", f"'{name}' קיימת. להחליף?") != \
                QMessageBox.StandardButton.Yes:
            return
        self.store.save_program(name, self.state)
        self._reload_combo(select=name)

    def _on_new(self) -> None:
        name, ok = QInputDialog.getText(self, "תוכנית חדשה", "שם התוכנית:")
        name = name.strip()
        if not ok or not name:
            return
        if self.store.exists(name):
            QMessageBox.warning(self, "קיים", f"'{name}' כבר קיימת.")
            return
        # Empty pattern set on all guns (config is kept as-is).
        self._loading = True
        for i in range(proto.NUM_GUNS):
            gp = self.state.patterns[i]
            gp.type = proto.PatternType.NONE
            gp.elements = []
            gp.on_timeout_ms = 1.2
            self.state.pattern_changed.emit(i, gp)
        self.store.save_program(name, self.state)
        self._reload_combo(select=name)
        self._loading = False
        self._refresh()

    def _on_delete(self) -> None:
        name = self.combo.currentText()
        if not name:
            return
        if QMessageBox.question(self, "מחיקה", f"למחוק את '{name}'?") != \
                QMessageBox.StandardButton.Yes:
            return
        self.store.delete(name)
        names = self.store.names()
        self._loading = True
        if not names:
            self.store.save_program(DEFAULT_NAME, self.state)
            names = self.store.names()
        nxt = names[0]
        self.store.apply(nxt, self.state)
        self._reload_combo(select=nxt)
        self._loading = False
        self._refresh()

    # ---- auto-save ---------------------------------------------------------
    def _schedule_autosave(self) -> None:
        if self._loading:
            return
        self._autosave.start()

    def _do_autosave(self) -> None:
        self.store.save_program(self.current_name(), self.state)
