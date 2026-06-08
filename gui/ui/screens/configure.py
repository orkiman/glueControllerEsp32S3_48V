"""Settings screen — all knobs in one place:

  - Solenoid currents + hold time (operator-facing)
  - Global motion params (photocell offset, min speed, debounce, pulses/mm)
  - Encoder calibration
  - Event log (with CSV export)
"""
from __future__ import annotations

import csv
import json
from datetime import datetime
from pathlib import Path

from PySide6.QtWidgets import (QCheckBox, QFileDialog, QFormLayout, QGridLayout,
                               QGroupBox, QHBoxLayout, QPlainTextEdit,
                               QPushButton, QVBoxLayout, QWidget)

from app.state import AppState, RuntimeConfig
from ui.widgets.numeric_field import NumericField


class ConfigureScreen(QWidget):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state
        self._log_rows: list[tuple[str, dict]] = []

        # ---- Operation params (currents — global, all guns) ----------------
        self.f_pick = NumericField("זרם משיכה (Pick)", "A", 0.1, 3.0, 0.05, 2)
        self.f_hold = NumericField("זרם החזקה (Hold)", "A", 0.05, 2.5, 0.05, 2)
        self.f_pick.bind(lambda v: state.push_config(pick_current_a=v))
        self.f_hold.bind(lambda v: state.push_config(hold_current_a=v))

        op_box = QGroupBox("זרמי סולנואיד (גלובלי לכל האקדחים)")
        op_form = QFormLayout(op_box)
        op_form.addRow(self.f_pick)
        op_form.addRow(self.f_hold)

        # ---- Globals -------------------------------------------------------
        self.f_offset   = NumericField("מרחק עין מתחילת הדף", "mm",
                                       0.0, 2000.0, 1.0, 1)
        self.f_min_spd  = NumericField("מהירות מינ' לפעולה", "mm/s",
                                       0.0, 2000.0, 10.0, 0)
        self.f_debounce = NumericField("Debounce עין", "ms",
                                       0, 1000, 1, integer=True)
        self.f_offset  .bind(lambda v: state.push_config(photocell_offset_mm=v))
        self.f_min_spd .bind(lambda v: state.push_config(min_speed_mm_s=v))
        self.f_debounce.bind(lambda v: state.push_config(debounce_ms=int(v)))

        glob_box = QGroupBox("הגדרות גלובליות")
        glob_form = QFormLayout(glob_box)
        glob_form.addRow(self.f_offset)
        glob_form.addRow(self.f_min_spd)
        glob_form.addRow(self.f_debounce)

        # ---- Calibration ---------------------------------------------------
        self.f_ppm   = NumericField("Pulses / mm", "",
                                    0.01, 1000.0, 0.01, 4)
        self.f_paper = NumericField("אורך דף ידוע (לקליברציה)", "mm",
                                    10.0, 2000.0, 1.0, 1)
        self.f_ppm.bind(lambda v: state.push_config(pulses_per_mm=v))
        self.btn_cal = QPushButton("דרוך קליברציה")
        self.btn_cal.clicked.connect(
            lambda: state.calibrate(self.f_paper.value()))

        cal_box = QGroupBox("קליברציית אנקודר")
        cal_form = QFormLayout(cal_box)
        cal_form.addRow(self.f_ppm)
        cal_form.addRow(self.f_paper)
        cal_form.addRow(self.btn_cal)

        # ---- Event log -----------------------------------------------------
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)

        self.chk_verbose = QCheckBox("הצג סטטוס שוטף")
        self.chk_verbose.setChecked(False)
        btn_clear  = QPushButton("נקה")
        btn_clear.clicked.connect(self._clear_log)
        btn_export = QPushButton("ייצוא CSV…")
        btn_export.clicked.connect(self._export_csv)
        log_actions = QHBoxLayout()
        log_actions.addWidget(self.chk_verbose)
        log_actions.addStretch(1)
        log_actions.addWidget(btn_clear)
        log_actions.addWidget(btn_export)

        log_box = QGroupBox("יומן אירועים")
        log_lay = QVBoxLayout(log_box)
        log_lay.addWidget(self.log, 1)
        log_lay.addLayout(log_actions)

        # ---- Compose -------------------------------------------------------
        top = QGridLayout()
        top.addWidget(op_box,   0, 0)
        top.addWidget(glob_box, 0, 1)
        top.addWidget(cal_box,  0, 2)

        root = QVBoxLayout(self)
        root.addLayout(top)
        root.addWidget(log_box, 1)

        state.config_changed.connect(self._on_config)
        state.log_appended.connect(self._on_log)
        state.command_sent.connect(self._on_command_sent)
        self._on_config(state.config)

    # ---- handlers ----------------------------------------------------------
    def _on_config(self, c: RuntimeConfig) -> None:
        self.f_pick    .setValue(c.pick_current_a)
        self.f_hold    .setValue(c.hold_current_a)
        self.f_offset  .setValue(c.photocell_offset_mm)
        self.f_min_spd .setValue(c.min_speed_mm_s)
        self.f_debounce.setValue(c.debounce_ms)
        self.f_ppm     .setValue(c.pulses_per_mm)

    def _is_routine(self, ev: dict) -> bool:
        """High-frequency, low-signal traffic that clutters the log:
        periodic status frames and the 1 Hz ping/ack keep-alive."""
        kind = ev.get("event", "")
        if kind == "status":
            return True
        if kind == "ack" and ev.get("cmd") == "ping":
            return True
        return False

    def _on_log(self, ev: dict) -> None:
        if self._is_routine(ev) and not self.chk_verbose.isChecked():
            return
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._log_rows.append((ts, ev))
        self.log.appendPlainText(f"[{ts}] {json.dumps(ev, ensure_ascii=False)}")

    def _on_command_sent(self, cmd: dict) -> None:
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        row = {"dir": "out", **cmd}
        self._log_rows.append((ts, row))
        self.log.appendPlainText(
            f"[{ts}] >> {json.dumps(cmd, ensure_ascii=False)}")

    def _clear_log(self) -> None:
        self._log_rows.clear()
        self.log.clear()

    def _export_csv(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "ייצוא יומן", "events.csv", "CSV (*.csv)")
        if not path:
            return
        with Path(path).open("w", encoding="utf-8-sig", newline="") as f:
            w = csv.writer(f)
            w.writerow(["timestamp", "event_json"])
            for ts, ev in self._log_rows:
                w.writerow([ts, json.dumps(ev, ensure_ascii=False)])
