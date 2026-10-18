"""Main window shell — header (run controls + connection, program bar) over
tabbed screens."""
from __future__ import annotations

from PySide6.QtWidgets import (QMainWindow, QSizePolicy, QTabWidget,
                               QVBoxLayout, QWidget)

from app import protocol as proto
from app.state import AppState
from ui.screens.configure import ConfigureScreen
from ui.screens.patterns import PatternsScreen
from ui.widgets.alert_banner import AlertBanner
from ui.widgets.connection_bar import ConnectionBar
from ui.widgets.program_bar import ProgramBar
from ui.widgets.run_panel import RunPanel, StateStrip

# Firmware safety trips: {"event":"error","cmd":"gunN"|"rt","reason":...}.
# Operator wording: what happened, then what to do.  Keep in sync with the
# tripReasons table in src/net/WebPage.h.
_TRIP_TEXT = {
    "pick_timeout":  "האקדח לא נפתח — הזרם בסליל לא עלה. "
                     "בדוק שהאקדח מחובר ושהכבל תקין. "
                     "אם האקדח לא בשימוש, הגדר את הסוג שלו ל'ללא'.",
    "decay_timeout": "הזרם בסליל לא ירד אחרי סגירה. בדוק את האקדח והכבל.",
    "close_late":    "טיפה נסגרה באיחור ונסגרה בכוח. אם זה חוזר — פנה לטכנאי.",
    "in1_stray":     "יציאת האקדח נמצאה פתוחה בלי סיבה ונסגרה. אם זה חוזר — פנה לטכנאי.",
    "tick_stalled":  "מערכת הבטיחות של הבקר נעצרה והופעלה מחדש. אם זה חוזר — פנה לטכנאי.",
}
_WARN_MS = 15000


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
        self.alert = AlertBanner()
        self._last_trip: dict[str, str] = {}   # "gunN" -> last trip reason

        v.addWidget(ConnectionBar(state, RunPanel(state)))
        v.addWidget(StateStrip(state))
        v.addWidget(self.program_bar)
        v.addWidget(self.alert)
        v.addWidget(body, 1)
        self.setCentralWidget(root)

        # status bar shows ping/connection text
        state.connection_changed.connect(self._on_conn)
        state.error_received.connect(self._on_error)
        state.status_changed.connect(self._on_status_alert)
        state.calib_result.connect(
            lambda ppm: self.alert.show_alert(
                f"קליברציה הושלמה: {ppm:.4f} pulses/mm — נשמר. "
                "המכונה חזרה למצב שהייתה בו לפני הקליברציה.", "info", _WARN_MS))

    def _on_conn(self, ok: bool, reason: str) -> None:
        self.statusBar().showMessage("מחובר" if ok else f"מנותק — {reason}")

    def _on_status_alert(self, s) -> None:
        # The stop message stays until the operator starts the machine again.
        if s.active:
            self.alert.clear("stop")
        if s.calibrating:
            if self.alert.level != "calib":
                self.alert.show_alert(
                    "מצב קליברציה — הדבק מושבת.\n"
                    "העבר דף אחד מול העין. הבקר סופר פולסים מרגע שהעין עולה "
                    "עד שהיא יורדת, ואז חוזר למצב הקודם (פעיל / לא פעיל).",
                    "calib")
        else:
            self.alert.clear("calib")     # finished or cancelled

    def _on_error(self, cmd: str, reason: str) -> None:
        if cmd == "calib_arm" and reason == "no_pulses":
            self.alert.show_alert(
                "קליברציה נכשלה — לא נספרו פולסים מהאנקודר בזמן שהדף עבר. "
                "בדוק שהמסוע זז ושמקור האנקודר בהגדרות נכון. "
                "הקליברציה הקודמת לא השתנתה.", "warn", _WARN_MS)
            return
        where = f"אקדח {cmd[3:]}" if cmd.startswith("gun") else "בקר"
        if reason == "safety_stop":
            # The firmware sends the trip that caused the stop just before it.
            cause = _TRIP_TEXT.get(self._last_trip.get(cmd, ""), "")
            self.alert.show_alert(
                f"המכונה נעצרה — {where}: תקלות בטיחות חוזרות.\n"
                f"{cause}\nאחרי התיקון לחץ 'הפעל'.", "stop")
            return
        trip = _TRIP_TEXT.get(reason)
        if trip:
            self._last_trip[cmd] = reason
            self.alert.show_alert(f"{where}: {trip}", "warn", _WARN_MS)
            return
        if reason == "pick_too_high":
            self.statusBar().showMessage(
                f"שגיאה: זרם משיכה מעל {proto.MAX_PICK_CURRENT_A}A — לא נשמר", 6000)
            return
        self.statusBar().showMessage(f"שגיאה: {cmd} → {reason}", 4000)

    def closeEvent(self, event) -> None:  # noqa: N802
        # Make sure the serial worker thread is stopped before the UI is torn
        # down; otherwise Qt prints "QThread destroyed while still running".
        self.state.link.close()
        event.accept()

    def _refresh_from_state(self) -> None:
        """Re-sync the pattern editor and toolbars from the current state
        (used after switching programs)."""
        for i in range(len(self.state.patterns)):
            gp = self.state.patterns[i]
            self.patterns_screen.editor.load_pattern(i, gp.type, gp.elements)
            self.patterns_screen.toolbars[i].set_type_from_state(gp.type)
            self.patterns_screen.toolbars[i].set_droplet_from_state(
                gp.on_timeout_ms)
