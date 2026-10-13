# Cold Glue Controller — PC GUI

Operator interface for the ESP32-S3 cold glue controller. Talks NDJSON
over USB serial (CP2102 @ 115200) to the firmware in `../src/`.

## Stack

- Python 3.11+
- PySide6 (Qt 6) — RTL Hebrew UI, QGraphicsView pattern editor
- pyserial — serial transport

## Install

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

## Run

```powershell
python run.py            # real serial
python run.py --mock     # in-process fake firmware, no hardware needed
```

## Standalone .exe (machine computer)

Build (once per GUI change):

```powershell
pip install pyinstaller          # inside the venv, one time
powershell -ExecutionPolicy Bypass -File build_exe.ps1
```

Output: `dist\GlueController.exe` — a single file, no Python needed on the
target.  The theme (`ui/styles.qss`) is bundled.

On the machine computer (Windows 10/11, 64-bit):

- Copy `GlueController.exe` into its own folder (e.g. `C:\GlueController\`);
  the logs are written next to it:
  - `glue.log` — always on, important events only (connect/disconnect,
    start/stop, setting / pattern / program changes, controller errors and
    safety trips, controller restarts and crash dumps).  Max 3 x 1 MB.
  - `debug.log` — only in debug mode (menu *קובץ → מצב דיבאג*, or
    `--debug`): all traffic including the 5 Hz status stream.  Debug mode is
    off at every start and the file is cleared when the program opens.
- First run: Windows SmartScreen may warn (the exe is unsigned) —
  *More info → Run anyway*.
- The CP210x USB-UART driver must be present (Windows Update usually installs
  it when the controller is plugged in).  The app auto-connects to the last
  used COM port; pick it once from the port list.
- Programs and settings live on the controller, so nothing else is copied.

## Layout

```
gui/
  run.py                       entry point
  app/
    protocol.py                NDJSON command builders + event parsing
    link_base.py               abstract link (Qt signals)
    serial_link.py             pyserial implementation
    mock_link.py               in-process fake firmware
    state.py                   central app state (config, patterns, status)
  ui/
    theme.py                   palette + QSS loader
    styles.qss                 dark industrial theme
    main_window.py             shell: header bars over tabbed screens
    widgets/
      connection_bar.py        header row: run panel + status LED + COM picker
      run_panel.py             start/stop, state, speed, sheet count (header)
      program_bar.py           program selector / save / delete
      numeric_field.py         labelled spinbox with units
      pattern_editor.py        QGraphicsView pattern canvas (TBD)
    screens/
      patterns.py              visual pattern editor host
      configure.py             per-gun currents + hold time
      admin.py                 calibration, globals, event log
```
