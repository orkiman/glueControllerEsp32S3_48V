"""Entry point for the Cold Glue Controller GUI."""
from __future__ import annotations

import argparse
import logging
import os
import sys

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import QApplication

from app.mock_link import MockLink
from app.serial_link import SerialLink
from app.state import AppState
from ui.main_window import MainWindow
from ui.theme import apply_dark_theme


def main() -> int:
    log_path = os.path.join(os.path.dirname(__file__), "debug.log")
    logging.basicConfig(
        filename=log_path,
        level=logging.DEBUG,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
        encoding="utf-8",
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
