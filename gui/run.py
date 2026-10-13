"""Entry point for the Cold Glue Controller GUI."""
from __future__ import annotations

import argparse
import logging
import os
import sys
from logging.handlers import RotatingFileHandler

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import QApplication

from app.mock_link import MockLink
from app.serial_link import SerialLink
from app.state import AppState
from ui.main_window import MainWindow
from ui.theme import apply_dark_theme


def main() -> int:
    # Frozen (PyInstaller .exe): __file__ is inside a temp folder deleted on
    # exit, so keep the log next to the .exe instead.
    base = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
            else os.path.dirname(__file__))
    log_path = os.path.join(base, "debug.log")
    # Status events are logged at 5 Hz all day: cap the log at 3 x 5 MB.
    handler = RotatingFileHandler(log_path, maxBytes=5 * 1024 * 1024,
                                  backupCount=2, encoding="utf-8")
    logging.basicConfig(
        handlers=[handler],
        level=logging.DEBUG,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
        force=True,
    )

    parser = argparse.ArgumentParser(description="Cold Glue Controller GUI")
    parser.add_argument("--mock", action="store_true",
                        help="Run with an in-process fake firmware (no serial).")
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setApplicationName("Cold Glue Controller")
    app.setLayoutDirection(Qt.RightToLeft)
    app.setFont(QFont("Segoe UI", 10))
    apply_dark_theme(app)

    link = MockLink() if args.mock else SerialLink()
    state = AppState(link)

    win = MainWindow(state)
    win.show()

    if args.mock:
        link.start()  # mock starts emitting status immediately

    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
