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

## Packaging (later)

```powershell
pyinstaller --noconsole --onefile run.py -n GlueController
```

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
    profiles.py                save/load profile JSONs
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
