"""Main window shell — header (run controls + connection, program bar) over
tabbed screens."""
from __future__ import annotations

from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import (QFileDialog, QMainWindow, QMessageBox,
                               QSizePolicy, QTabWidget, QVBoxLayout, QWidget)

from app import profiles
from app.state import AppState
from ui.screens.configure import ConfigureScreen
from ui.screens.patterns import PatternsScreen
from ui.widgets.connection_bar import ConnectionBar
from ui.widgets.program_bar import ProgramBar
from ui.widgets.run_panel import RunPanel


class MainWindow(QMainWindow):
    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state
        self.setWindowTitle("בקר דבק קר — תוכנת הפעלה")
        self.resize(1200, 760)
        self.setMinimumSize(800, 500)
        self.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Preferred)

        # ---- screens ----
        self.tabs = QTabWidget()
        self.patterns_screen = PatternsScreen(state)
        self.tabs.addTab(self.patterns_screen, "תוכנית")
        self.tabs.addTab(ConfigureScreen(state), "הגדרות")

        self._build_menu()

        # ---- header: run controls + connection, then program bar ----
        self.program_bar = ProgramBar(state, self._refresh_from_state)
        self.program_bar.setObjectName("ProgramBar")

        body = QWidget()
        body_lay = QVBoxLayout(body)
        body_lay.setContentsMargins(10, 10, 10, 10)
        body_lay.addWidget(self.tabs)

        root = QWidget()
        v = QVBoxLayout(root)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(0)
        v.addWidget(ConnectionBar(state, RunPanel(state)))
        v.addWidget(self.program_bar)
        v.addWidget(body, 1)
        self.setCentralWidget(root)

        # status bar shows ping/connection text
        state.connection_changed.connect(self._on_conn)
        state.error_received.connect(self._on_error)

    def _on_conn(self, ok: bool, reason: str) -> None:
        self.statusBar().showMessage("מחובר" if ok else f"מנותק — {reason}")

    def _on_error(self, cmd: str, reason: str) -> None:
        self.statusBar().showMessage(f"שגיאה: {cmd} → {reason}", 4000)

    def closeEvent(self, event) -> None:  # noqa: N802
        # Make sure the serial worker thread is stopped before the UI is torn
        # down; otherwise Qt prints "QThread destroyed while still running".
        self.state.link.close()
        event.accept()

    # ---- menu ---------------------------------------------------------------
    def _build_menu(self) -> None:
        mb = self.menuBar()
        m_file = mb.addMenu("&קובץ")

        act_load = QAction("טען פרופיל…", self)
        act_load.setShortcut(QKeySequence.Open)
        act_load.triggered.connect(self._load_profile)
        m_file.addAction(act_load)

        act_save = QAction("שמור פרופיל…", self)
        act_save.setShortcut(QKeySequence.Save)
        act_save.triggered.connect(self._save_profile)
        m_file.addAction(act_save)

        m_file.addSeparator()
        act_quit = QAction("יציאה", self)
        act_quit.setShortcut(QKeySequence.Quit)
        act_quit.triggered.connect(self.close)
        m_file.addAction(act_quit)

    def _save_profile(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "שמירת פרופיל", "profile.json", "JSON (*.json)")
        if not path:
            return
        try:
            profiles.save_to(path, self.state)
            self.statusBar().showMessage(f"נשמר: {path}", 4000)
        except OSError as exc:
            QMessageBox.warning(self, "שגיאת שמירה", str(exc))

    def _load_profile(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "טעינת פרופיל", "", "JSON (*.json)")
        if not path:
            return
        try:
            profiles.load_from(path, self.state)
        except (OSError, ValueError) as exc:
            QMessageBox.warning(self, "שגיאת טעינה", str(exc))
            return
        self._refresh_from_state()
        self.statusBar().showMessage(f"נטען: {path}", 4000)

    def _refresh_from_state(self) -> None:
        """Re-sync the pattern editor and toolbars from the current state
        (used after loading a profile file or switching programs)."""
        for i in range(len(self.state.patterns)):
            gp = self.state.patterns[i]
            self.patterns_screen.editor.load_pattern(i, gp.type, gp.elements)
            self.patterns_screen.toolbars[i].set_type_from_state(gp.type)
            self.patterns_screen.toolbars[i].set_droplet_from_state(
                gp.on_timeout_ms)
