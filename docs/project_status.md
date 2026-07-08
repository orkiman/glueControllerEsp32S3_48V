# Cold Glue Controller — Project Status

> Living document. Reflects what is **actually implemented** in firmware.
> The original spec lives in `initial prompt.md` and should not be edited;
> this file records any decisions, deltas, and the current state.

Last updated: Jun 2026. Stage 2 GUI feature-complete (mock-verified).
Per-gun `on_timeout_ms`, multi-program management, event log filtering,
dynamic canvas, and overlap prevention all landed.

---

## 1. Stage Status

| Stage | Scope                                          | Status            |
| ----- | ---------------------------------------------- | ----------------- |
| 1     | ESP32-S3 main controller firmware              | **Code complete** — pending hardware bring-up |
| 2     | PC-side GUI over UART0 (PySide6)               | **Feature complete** — mock-verified; pending first real-hardware run |

The original Stage 2 plan was an LVGL HMI on a second ESP32-S3 + 7" touch
panel. **Revised decision:** for now the operator UI runs on a PC and talks
to the main controller through the on-board CP2102 USB-UART. The wire
protocol (NDJSON over UART0 @ 115200) is identical, so swapping back to an
embedded HMI later is a drop-in.

---

## 2. Implemented Architecture

### Dual-core task layout

| Core | Task        | Prio | File                              | Purpose                                |
| ---- | ----------- | ---- | --------------------------------- | -------------------------------------- |
| 0    | `uart_rx`   | 4    | `comms/UartJson.cpp`              | NDJSON parser + command dispatcher     |
| 0    | `evt_emit`  | 5    | `comms/Events.cpp`                | Drains lock-free event queue -> Serial |
| 0    | `wdog`      | 3    | `sys/Watchdog.cpp`                | HMI link timeout (2 s)                 |
| 0    | `status`    | 2    | `sys/Status.cpp`                  | 5 Hz `{"event":"status"}`              |
| 1    | `pattern`   | 7    | `rt/PatternScheduler.cpp`         | Encoder poll + fire pattern events     |
| 1    | `dac`       | 6    | `hw/Dac.cpp`                      | Coalesced MCP4728 I2C writes           |
| 1    | ISRs        | IRAM | `hw/Encoder`, `rt/GunSequencer`, `sys/Fault` | Peak (x4), photocell, nFAULT, PCNT overflow |

### Key isolation rules (enforced by file layout)

- Only `evt_emit` task ever touches `Serial`. No other module may print.
- ISRs never block on I2C: DAC writes are queued to the Core-1 `dac` task.
- Config is published via atomic double-buffer pointer swap; hot path reads
  are lock-free.
- Cross-core events use FreeRTOS queues with `*FromISR` APIs.

---

## 3. Dot Sequence — Implementation Map

| Phase                | Trigger                          | File / function                                          |
| -------------------- | -------------------------------- | -------------------------------------------------------- |
| 1 Peak / Pick        | `seq::fire(g)`                   | `rt/GunSequencer.cpp :: fire()`                          |
| 2 Hold (chopping)    | LM339 peak IRQ                   | `rt/GunSequencer.cpp :: peakIsr()`                       |
| 3 Active Fast Decay  | on-timer expiry OR `seq::abort()`| `rt/GunSequencer.cpp :: onTimerCb() / abort()`           |

Per-gun state machine is `Idle -> Peak -> Hold -> Decay -> Idle`, guarded
by a `compare_exchange_strong` so `fire()` is safe from any context.

### On-timer (Peak+Hold budget)

`fire()` arms a single `esp_timer` (`onTimer`, per gun) for the full
per-gun `on_timeout_ms` budget, starting **at the moment of fire()**, not
at the LM339 peak trip.  This means:

- Normal flow: LM339 -> `peakIsr` -> Hold phase; the same timer keeps
  counting and forces Phase 3 when the budget expires.
- Fault flow: if the peak trip never arrives (open coil, broken sense,
  wrong threshold), the same timer still fires, drops the gun into
  Phase 3, and masks the late IRQ defensively.  No more "stuck in Peak"
  failure mode.
- `seq::abort(g)` stops the on-timer and force-enters Phase 3 (used by
  Lines to terminate at the encoder-position end of the line).

---

## 4. Pattern Execution

- Per-gun **sheet FIFO depth = 4** (`SHEET_QUEUE_DEPTH` in `PatternScheduler.cpp`).
- Sheet entries store only the encoder pulse count at the photocell leading
  edge. Patterns are walked **lazily** at runtime — dots are not pre-expanded.
- Pattern coordinate origin = paper leading edge =
  `photocell pulse + photocell_offset_mm * pulses_per_mm`.
- If gap between sheets < `photocell_offset_mm`, multiple sheets are tracked
  in flight on the same gun. Queue overflow drops the new trigger silently.
- Speed-safety: if measured speed drops below `min_speed_mm_s`, the pattern
  task suppresses firing until speed recovers.

---

## 5. Diagnostic `test_open` Behaviour (revised)

`test_open` no longer issues a single raw dot. Behaviour depends on the gun's
currently configured pattern type (see `rt/TestRunner.cpp`):

| Pattern type | Test behaviour                                                       |
| ------------ | -------------------------------------------------------------------- |
| `lines`      | Single line, hardware-regulated, held for `timeout_ms`, then Phase 3 |
| `dots`       | Continuous dot train at 20 Hz until `timeout_ms` or `test_close`     |
| `none`       | `{"event":"error","cmd":"test_open","reason":"no_pattern_or_busy"}`  |

`gun:0` broadcasts the test to every gun with a configured pattern.
`timeout_ms` is capped at 5000 in `UartJson::handleTestOpen`.

---

## 6. Fault & Safety Path

- **nFAULT (GPIO 39, active LOW)**: IRAM ISR in `sys/Fault.cpp` immediately
  calls `drv::killAll()`, aborts all gun sequencers, sets
  `g_sys.fault = true`, `g_sys.active = false`, and emits
  `{"event":"error","reason":"hardware_fault"}`.
- Recovery requires a fresh `set_active:true` from the operator.
- **Watchdog**: any received NDJSON command (incl. `ping`) refreshes the
  timestamp. After 2 s of silence while active, outputs are killed and
  `{"event":"watchdog_timeout"}` is emitted.

---

## 7. Protocol — Deltas From `initial prompt.md` §8

### 7.1 New error reason

| Where          | Reason                  | Meaning                                                 |
| -------------- | ----------------------- | ------------------------------------------------------- |
| `test_open`    | `no_pattern_or_busy`    | Gun has no pattern, or a test is already running on it. |
| `set_pattern`  | `bad_on_timeout`        | `on_timeout_ms` was supplied but ≤ 0.                   |

All other validation errors (`bad_pulses_per_mm`, `hold_ge_pick`, etc.) are
sanity checks in `set_config` / `set_pattern`.

### 7.2 `on_timeout_ms` — per-gun, start-of-cycle

Previously a global `hold_time_ms` in `RuntimeConfig` was started by
`peakIsr` and counted only the Hold phase.  It has been replaced by a
per-gun `on_timeout_ms` inside `GunPattern`, started by `fire()` itself
and counting the **entire** Peak+Hold budget.

- `set_config` **no longer accepts** any droplet-timing field.
- `set_pattern` accepts a top-level optional `on_timeout_ms` (ms, float).
  Absent → preserve previous per-gun value (`editScratch()` copies the
  active buffer).
- Default per-gun value at boot: **1.2 ms**.
- **Dots mode**: `on_timeout_ms` is the user-facing droplet on-time.
- **Lines mode**: `on_timeout_ms` is used only as a long safety ceiling
  (`seq::fire(g, 5000)` is hard-capped to 5 s inside `fire()`).  Line
  termination is encoder-position driven (`seq::abort(g)` at `end_mm`,
  see `PatternScheduler::patternTask`).  Speed-safety (`min_speed_mm_s`)
  still suppresses firing entirely.
- **No more "stuck in Peak"**: because the on-timer starts at `fire()`,
  a missing LM339 trip cannot pin IN1 HIGH indefinitely.

Example payload:

```json
{ "cmd": "set_pattern", "gun": 2, "type": "dots",
  "on_timeout_ms": 1.8,
  "elements": [ {"start": 60, "end": 240, "spacing": 5} ] }
```

---

## 8. GUI — Implemented Features (Stage 2 summary)

| Feature | Notes |
| ------- | ----- |
| Multi-program management | `app/programs.py` + `ui/widgets/program_bar.py` — dropdown, save, save-as, delete, auto-save, auto-load last |
| Event log filtering | Routine `status` and `ping_ack` events hidden by default; opt-in checkbox to show them |
| Outbound command log | All `_send()` calls (except ping) appear in the event log |
| Dynamic canvas | Canvas grows automatically when a segment is dragged or added outside current bounds |
| Overlap prevention | Drag, handle resize, and double-click add all prevent segment overlap |
| Hebrew RTL fixes | Group-box title padding widened; lane labels use document margin |
| Encoder calibration | `calib_arm` command + `calib_result` event updates `pulses_per_mm` live |
| Dark RTL theme | `styles.qss` + `Qt.RightToLeft` layout direction |
| Live pattern type push | Toolbar combo changes push `set_pattern` immediately to the firmware |
| `set_pattern type:none` | Firmware now accepts `none` to clear/disable a gun |
| COM port persistence | Dropdown auto-selects the last port, switches ports when changed, and saves the last connected port |
| Auto-sync on reconnect | Config + all patterns are pushed automatically when the serial link opens, so startup program loads are not lost |
| Resync on firmware reboot | GUI re-pushes full state whenever a `ready` event arrives (USB-CDC link survives an ESP reboot, so the connect signal never drops) |
| ISR-safe abort (firmware) | `seq::abort` no longer calls `esp_timer_stop` from `faultIsr` (ISR context) — this was panicking/rebooting the ESP32 on every `test_open` when nFAULT asserted (e.g. DRV8262 with no 48 V) |
| Clean shutdown | `MainWindow.closeEvent` closes the serial thread before exit to avoid QThread warnings |
| Test button safety | Test button disabled when disconnected or system inactive; shows "עצור בדיקה" while running; auto-releases on timeout/error |
| `test_open` errors | `not_active` / `no_pattern` / `busy` / `hardware_fault` (previously all collapsed into `no_pattern_or_busy`) |

To run:
```powershell
# Mock (no hardware needed):
cd gui
.venv\Scripts\python run.py --mock

# Real hardware:
.venv\Scripts\python run.py
```

---

## 9. Open Items For Bench Bring-Up

- **`Adafruit_MCP4728::fastWrite` signature** — assumed `(uint16_t, uint16_t, uint16_t, uint16_t)` returning `bool`; will fail at compile if the installed lib differs.
- **PCNT glitch filter** at 100 APB ticks (~1.25 µs). Will tune once we see the 6N137's real edge behaviour on the scope.
- **`NEAR_ZERO_V = 0.10 V`** Phase-3 termination threshold. Tunable in `rt/GunSequencer.cpp`. Right now corresponds to ~0.05 A coil current.
- **On-timer dispatch via `esp_timer` task context** (not ISR). Worst-case latency ~50 µs; fine for on-times ≥ 1 ms but introduces jitter if `on_timeout_ms` is set absurdly small.
- **No `error` event yet for the "peak trip never arrived" case** — the
  on-timer correctly drops the gun into Phase 3 (no stuck-in-Peak any
  more), but we still can't tell from outside whether a given fire cycle
  passed through `peakIsr` or hit the on-timeout without it.  *Future:*
  emit a diagnostic event from `onTimerCb` if the gun was still in
  `Phase::Peak` when the timer expired.

---

## 10. File Tree (current)

```
platformio.ini
README.md
docs/
  initial prompt.md            (immutable spec — do not edit)
  project_status.md            (this file)
src/                           (ESP32-S3 firmware)
  main.cpp
  config/Config.{h,cpp}
  comms/Events.{h,cpp}    UartJson.{h,cpp}
  hw/Pins.h               Driver.{h,cpp}   Dac.{h,cpp}   Encoder.{h,cpp}
  rt/Control.{h,cpp}      GunSequencer.{h,cpp}   PatternScheduler.{h,cpp}   TestRunner.{h,cpp}
  sys/Fault.{h,cpp}       Watchdog.{h,cpp}       Status.{h,cpp}
gui/                           (PySide6 operator UI)
  run.py                       Launcher (`--mock` for offline dev)
  requirements.txt
  app/
    protocol.py                NDJSON command builders + enums
    link_base.py               Abstract link (Qt signals)
    mock_link.py               In-process simulator
    serial_link.py             pyserial-backed real transport
    state.py                   AppState — single source of truth
    profiles.py                Save/load full operator profile (JSON)
    programs.py                Persistent named-program store (auto-save/load)
  ui/
    main_window.py             Sidebar (הפעלה / תוכנית / הגדרות) + menu
    styles.qss                 Dark theme
    screens/operate.py         Run-time controls + live status
    screens/patterns.py        Per-gun toolbar + visual editor (4 lanes)
    screens/configure.py       Currents, globals, calibration, event log
    widgets/connection_bar.py  COM picker / Connect / Active / E-stop
    widgets/numeric_field.py   Debounced labelled spinbox
    widgets/pattern_editor.py  QGraphicsScene-based segment editor
    widgets/program_bar.py     Program selector / save-as / delete toolbar
```
