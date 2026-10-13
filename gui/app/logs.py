"""GUI logging.

- glue.log  (always on): important events only -- connect/disconnect,
  start/stop, setting / pattern / program changes, controller errors, safety
  trips, controller restarts and crash dumps.  Small: 3 x 1 MB.
- debug.log (debug mode only): everything, including the 5 Hz status
  stream.  Debug mode is off at every start and the file is cleared when the
  program opens, so it only ever holds the current session.

Log importance where the message is written: INFO / WARNING for the regular
log, DEBUG for traffic that only belongs in the debug log.
"""
from __future__ import annotations

import logging
import os
from logging.handlers import RotatingFileHandler

MAIN_LOG  = "glue.log"
DEBUG_LOG = "debug.log"

_FORMAT = "%(asctime)s %(levelname)s %(name)s %(message)s"
_dir = "."
_debug_handler: logging.Handler | None = None


def setup(log_dir: str, debug: bool = False) -> None:
    global _dir
    _dir = log_dir
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
        h.close()
    root.setLevel(logging.DEBUG)

    # Previous session's debug log (and old rotated copies from earlier
    # versions, which logged everything here) -- start clean.
    for name in (DEBUG_LOG, DEBUG_LOG + ".1", DEBUG_LOG + ".2"):
        try:
            os.remove(os.path.join(log_dir, name))
        except OSError:
            pass

    main = RotatingFileHandler(os.path.join(log_dir, MAIN_LOG),
                               maxBytes=1024 * 1024, backupCount=2,
                               encoding="utf-8")
    main.setLevel(logging.INFO)
    main.setFormatter(logging.Formatter(_FORMAT))
    root.addHandler(main)
    logging.getLogger("app").info("program started")

    if debug:
        set_debug(True)


def debug_path() -> str:
    return os.path.join(_dir, DEBUG_LOG)


def debug_enabled() -> bool:
    return _debug_handler is not None


def set_debug(on: bool) -> None:
    global _debug_handler
    root = logging.getLogger()
    if on and _debug_handler is None:
        # Append: turning debug off and on again keeps this session's lines.
        _debug_handler = logging.FileHandler(debug_path(), mode="a",
                                             encoding="utf-8")
        _debug_handler.setLevel(logging.DEBUG)
        _debug_handler.setFormatter(logging.Formatter(_FORMAT))
        root.addHandler(_debug_handler)
        logging.getLogger("app").info("debug mode on -> %s", debug_path())
    elif not on and _debug_handler is not None:
        logging.getLogger("app").info("debug mode off")
        root.removeHandler(_debug_handler)
        _debug_handler.close()
        _debug_handler = None
