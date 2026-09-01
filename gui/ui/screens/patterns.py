"""Patterns screen — per-gun toolbar + multi-lane visual editor."""
from __future__ import annotations

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QComboBox, QGridLayout, QGroupBox, QHBoxLayout,
                               QLabel, QPushButton, QVBoxLayout, QWidget)

from app import protocol as proto
from app.state import AppState, GunPattern
from ui.widgets.numeric_field import NumericField
from ui.widgets.pattern_editor import GUN_COLORS, PatternEditorView


_TYPE_LABEL = {
    proto.PatternType.NONE:  "ללא",
    proto.PatternType.LINES: "קווים",
    proto.PatternType.DOTS:  "נקודות",
}


class GunToolbar(QGroupBox):
    """Per-gun compact toolbar above the editor."""

    def __init__(self, gun_idx: int, state: AppState,
                 editor: PatternEditorView) -> None:
        super().__init__(f"אקדח {gun_idx + 1}")
        self.gun_idx = gun_idx
        self.state   = state
        self.editor  = editor

        color = GUN_COLORS[gun_idx]
        self.setStyleSheet(
            f"QGroupBox::title {{ color: {color.name()}; font-weight: 700; }}")

        self.type_combo = QComboBox()
        for t in (proto.PatternType.NONE,
                  proto.PatternType.LINES,
                  proto.PatternType.DOTS):
            self.type_combo.addItem(_TYPE_LABEL[t], t)
        self.type_combo.currentIndexChanged.connect(self._on_type_changed)

        # Per-gun on-timeout budget. In Dots mode this is the droplet size
        # (user-facing label "גודל טיפה"). In Lines mode the firmware uses
        # it only as a long safety ceiling, so we grey the field out there.
        self.f_droplet = NumericField("גודל טיפה (זמן ON)", "ms",
                                      0.2, 50.0, 0.1, 2)
        self.f_droplet.bind(self._on_on_timeout_changed)

        btn_add  = QPushButton("הוסף")
        btn_add.setToolTip("הוסף מקטע חדש")
        btn_add.clicked.connect(lambda: editor.add_segment(gun_idx))
        btn_clr  = QPushButton("נקה")
        btn_clr.setToolTip("נקה את כל המקטעים של האקדח")
        btn_clr.clicked.connect(lambda: editor.clear_gun(gun_idx))
        btn_test = QPushButton("בדיקה")
        btn_test.setToolTip("הפעל/עצור בדיקת אקדח")
        btn_test.setCheckable(True)
        btn_test.toggled.connect(self._on_test_toggled)
        self.btn_test = btn_test

        # The firmware does not emit a dedicated "test finished" event, so we
        # use the button checked state as the running indicator and release it
        # when the chosen timeout expires or an error is reported.
        self._test_timer = QTimer(self)
        self._test_timer.setSingleShot(True)
        self._test_timer.timeout.connect(self._release_test_button)
        state.error_received.connect(self._on_test_error)
        state.connection_changed.connect(self._update_test_enabled)
        state.status_changed.connect(self._update_test_enabled)

        type_row = QHBoxLayout()
        type_row.addWidget(QLabel("סוג:"))
        type_row.addWidget(self.type_combo, 1)

        btn_row = QHBoxLayout()
        btn_row.setSpacing(6)
        btn_row.addWidget(btn_add)
        btn_row.addWidget(btn_clr)
        btn_row.addWidget(btn_test)

        outer = QVBoxLayout(self)
        outer.setSpacing(8)
        outer.addLayout(type_row)
        outer.addLayout(btn_row)
        outer.addWidget(self.f_droplet)

    def set_type_from_state(self, ptype: proto.PatternType) -> None:
        idx = self.type_combo.findData(ptype)
        if idx >= 0:
            self.type_combo.blockSignals(True)
            self.type_combo.setCurrentIndex(idx)
            self.type_combo.blockSignals(False)
        # Droplet field is meaningful only for Dots — disable otherwise.
        self.f_droplet.setEnabled(ptype == proto.PatternType.DOTS)
        self._update_test_enabled()

    def _update_test_enabled(self, _ok: bool = False, _reason: str = "") -> None:
        enabled = (self.state.link.connected and
                   self.state.status.active and
                   self.state.patterns[self.gun_idx].type != proto.PatternType.NONE)
        self.btn_test.setEnabled(enabled)

    def set_droplet_from_state(self, on_timeout_ms: float) -> None:
        self.f_droplet.setValue(on_timeout_ms)

    def _on_type_changed(self, _i: int) -> None:
        ptype: proto.PatternType = self.type_combo.currentData()
        self.editor.set_gun_type(self.gun_idx, ptype)
        self.f_droplet.setEnabled(ptype == proto.PatternType.DOTS)

        gp = self.state.patterns[self.gun_idx]
        gp.type = ptype
        gp.elements = [] if ptype == proto.PatternType.NONE else [
            e for e in self.editor.export_pattern(self.gun_idx)[1]
        ]
        self.state.push_pattern(self.gun_idx)
        self._update_test_enabled()
        if ptype == proto.PatternType.NONE:
            self._release_test_button()

    def _on_on_timeout_changed(self, v: float) -> None:
        self.state.patterns[self.gun_idx].on_timeout_ms = float(v)
        # Push only if the pattern actually exists on the firmware.
        if self.state.patterns[self.gun_idx].type != proto.PatternType.NONE:
            self.state.push_pattern(self.gun_idx)

    def _on_test_toggled(self, on: bool) -> None:
        gun_1based = self.gun_idx + 1
        self.btn_test.setText("עצור" if on else "בדיקה")
        if on:
            self.state.test_open(gun_1based, timeout_ms=5000)
            self._test_timer.start(5000)
        else:
            self._test_timer.stop()
            self.state.test_close(gun_1based)

    def _release_test_button(self) -> None:
        self._test_timer.stop()
        if self.btn_test.isChecked():
            self.btn_test.blockSignals(True)
            self.btn_test.setChecked(False)
            self.btn_test.blockSignals(False)
        self.btn_test.setText("בדיקה")

    def _on_test_error(self, cmd: str, _reason: str) -> None:
        if cmd == "test_open":
            self._release_test_button()


class PatternsScreen(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state  = state
        self.editor = PatternEditorView()

        grid = QGridLayout()
        grid.setHorizontalSpacing(8)
        self.toolbars: list[GunToolbar] = []
        for i in range(proto.NUM_GUNS):
            tb = GunToolbar(i, state, self.editor)
            self.toolbars.append(tb)
            grid.addWidget(tb, 0, i)

        hint = QLabel(
            "טיפים: לחיצה כפולה על נתיב = הוספת מקטע · "
            "גרירת ידיות = שינוי גבולות · גרירת גוף = הזזה · "
            "גלגלת על מקטע נקודות = שינוי מרווח · "
            "קליק ימני על מקטע = מחיקה")
        hint.setObjectName("StatCaption")

        root = QVBoxLayout(self)
        root.addLayout(grid)
        root.addWidget(self.editor, 1)
        root.addWidget(hint)

        # Live publish wiring (editor -> firmware via state).
        self.editor.pattern_committed.connect(self._on_pattern_committed)
        state.config_changed.connect(self._on_config)
        state.pattern_changed.connect(self._on_pattern_changed)
        # Initial sync.
        for i in range(proto.NUM_GUNS):
            self.toolbars[i].set_type_from_state(state.patterns[i].type)
            self.toolbars[i].set_droplet_from_state(
                state.patterns[i].on_timeout_ms)
            self.editor.load_pattern(i, state.patterns[i].type,
                                     state.patterns[i].elements)

    def _on_pattern_changed(self, gun_idx: int, gp: GunPattern) -> None:
        self.editor.load_pattern(gun_idx, gp.type, gp.elements)
        self.toolbars[gun_idx].set_type_from_state(gp.type)
        self.toolbars[gun_idx].set_droplet_from_state(gp.on_timeout_ms)

    def _on_pattern_committed(self, gun_idx: int) -> None:
        ptype, elems = self.editor.export_pattern(gun_idx)
        gp: GunPattern = self.state.patterns[gun_idx]
        gp.type     = ptype
        gp.elements = list(elems)
        # Keep toolbar in sync if the editor changed the type itself.
        self.toolbars[gun_idx].set_type_from_state(ptype)
        # Live push (push_pattern itself short-circuits if type is NONE).
        self.state.push_pattern(gun_idx)

    def _on_config(self, _c) -> None:
        # Future: drive paper length from config if we add a field for it.
        pass
