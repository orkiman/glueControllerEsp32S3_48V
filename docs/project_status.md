# Cold Glue Controller — Project Status

> Living document. Reflects what is **actually implemented** in firmware.
> The original spec lives in `initial prompt.md` and should not be edited;
> this file records any decisions, deltas, and the current state.

Last updated: Oct 2026. Real-time safety supervisor (hardware close timer,
pick limit, safety trips), drops fired on the exact encoder pulse, DAC pick
pre-arm.  Not yet run on the machine -- see §9 checklist.

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
| 0    | `rt_check` (esp_timer, 1 ms) | 22 | `rt/GunSequencer.cpp`   | Last-resort close, IN1 readback, supervisor watchdog, peak diag events |
| 1    | `pattern`   | 7    | `rt/PatternScheduler.cpp`         | Fires pattern events; woken by the supervisor at the due pulse |
| 1    | `dac`       | 6    | `hw/Dac.cpp`                      | MCP4728 I2C writes (changed channels only) |
| 1    | Close alarm (TIMERG0 T0) | IRAM | `hw/RtTimers`, `rt/GunSequencer` | Drop end at the exact on-time |
| 1    | Supervisor (TIMERG1 T0, 50 us) | IRAM | `hw/RtTimers`, `rt/GunSequencer`, `rt/PatternScheduler` | Safety checks, due-pulse wake-up, line ends |
| 1    | ISRs        | IRAM | `hw/Encoder`, `rt/GunSequencer`, `sys/Fault` | Peak (x4), photocell, nFAULT, PCNT overflow |

### Key isolation rules (enforced by file layout)

- Only `evt_emit` task ever touches `Serial`. No other module may print.
- ISRs never block on I2C: DAC writes are queued to the Core-1 `dac` task.
- Config is published via atomic double-buffer pointer swap; hot path reads
  are lock-free.
- Cross-core events use FreeRTOS queues with `*FromISR` APIs.
- **ISRs are IRAM-only.**  GPIO/PCNT ISRs are installed with
  `ESP_INTR_FLAG_IRAM`, so they keep running while Core 0 writes flash
  (program/config autosave), when the flash cache is off.  Anything an ISR
  touches must be in IRAM/DRAM: no `gpio_*` / `pcnt_*` driver calls (use
  direct register access), no `constexpr` tables or string literals (flash
  `.rodata` -> `DRAM_ATTR`), and no FPU.  Violations crash with
  *"Cache disabled but cached memory region accessed"* a few seconds after a
  settings change, and the controller reboots inactive (seen 2026-10-06,
  fixed).  To check after every firmware change: `py tools/check_isr_iram.py`
  (walks everything reachable from the interrupts in the built ELF and
  fails on any call or `l32r` literal pointing into 0x42xxxxxx / 0x3Cxxxxxx).
  Avoid in interrupt code: sub-word `std::atomic` read-modify-write (libcall),
  `switch` jump tables, 64-bit division.
- During a flash write Core 1 tasks stop (Core 1 spins in an IPC task); only
  IRAM interrupts run.  That is why drop ends, line ends and every safety
  check run in the timer interrupts, not in tasks.

---

## 3. Dot Sequence — Implementation Map

| Phase                | Trigger                          | File / function                                          |
| -------------------- | -------------------------------- | -------------------------------------------------------- |
| 1 Peak / Pick        | `seq::fire(g)` (task only)       | `rt/GunSequencer.cpp :: fire()`                          |
| 2 Hold (chopping)    | LM339 peak IRQ                   | `rt/GunSequencer.cpp :: peakIsr()`                       |
| 3 Close / decay      | close alarm, line end, `abort()`, safety trip | `rt/GunSequencer.cpp :: closeLocked()`      |

Per-gun state `Idle -> Peak -> Hold -> Idle` (IN1 high exactly while not
Idle), plus a `decaying` flag after a close.  All transitions happen under
one spinlock, so the interrupts, the pattern task (Core 1) and Core 0
callers (stop, watchdog, test runner) see a consistent state.

- **fire()**: refuses while the gun is still open -- a new drop that comes
  before the previous one closed is ignored.  Needs the pick threshold on
  the DAC (normally pre-armed, else written over I2C first; refuses if that
  fails).  Arms the close **before** IN1 goes high, then MUX_SELECT=1, peak
  IRQ on, IN1=1.  Returns a drop id.
- **On-time** (`on_timeout_ms`, or the caller's value for lines/tests)
  counts from fire(), not from the peak trip.  If it ends before the peak,
  the gun closes (normal for short dots).
- **Close**: IN1=0, DAC -> near-zero (0.1 V = 0.05 A).  MUX_SELECT stays 1
  so the LM339 reverse-drives the coil to ~0 A, as before.  When the
  supervisor sees near-zero confirmed on the DAC and the comparator low for
  2 ticks, MUX_SELECT goes 0 (IN2 held low by the ESP32 between drops) and
  the pick threshold is pre-armed on the DAC for the next drop.
- **Ways a drop closes** (independent): close alarm interrupt at the exact
  end; supervisor 100 us after the end; Core 0 `rt_check` 2 ms after.

### 3a. Safety supervisor (50 us, interrupt, Core 1)

| Check | Action | Reported reason |
| ----- | ------ | --------------- |
| Drop still open 100 us after its end (close alarm missed) | close | `close_late` |
| Hold threshold not confirmed on the DAC 3 ms after opening (missed peak trip, dead sense, failed/late I2C) | close | `pick_timeout` |
| IN1 high with no open drop (readback) | force low | `in1_stray` |
| Near-zero on the DAC, comparator still high 2 ms later | MUX_SELECT=0 | `decay_timeout` |
| Near-zero never confirmed within 5 ms of the close (DAC late) | MUX_SELECT=0 (coast) | -- |
| Supervisor tick stopped (checked by `rt_check`) | restart it | `tick_stalled` (cmd `rt`) |

Each trip is `{"event":"error","cmd":"gunN","reason":...}`.  Three trips in
a row on one gun (no good drop in between, < 60 s apart) stop the machine
like a stop command: `{"event":"error","cmd":"gunN","reason":"safety_stop"}`.
`set_active:true` clears the streaks.  `/api/status` reports `trips`,
`last_trip_gun`, `last_trip`; the GUI status bar and the web page show them.

Remaining limit: with dots, a dead current sense cannot be detected ("no
peak" is normal for short dots), so each dot runs up to its on-time
unregulated -- the DRV8262's own limit (VREF 3.3 V, IPROPI 5.1 k ->
ITRIP ~3 A) still caps the current.

### 3b. DAC

- Only changed channels are written: Multi-Write for 1-2 channels (4 / 7
  bytes), Fast Write for 3-4 (8 bytes), 400 kHz.  `isApplied()` reports
  what is confirmed on the chip.  Failed writes retry at once and then every
  tick; `i2c_write_failed` at most once per second.
- The chip EEPROM (loaded at power-up) holds 0.10 V on every channel (set
  once at boot if different), so at power-up the comparators read "no
  current".  The MUX select / IN0 lines have no pull-downs on the board and
  float until the firmware starts; this covers a MUX that floats to the
  comparator side (gun 4's select, GPIO48, has a weak pull-up at reset).
- Pick current max 1.5 A (`cfg::MAX_PICK_CURRENT_A`): the sense gives 2 V/A
  on a 3.3 V supply, so above ~1.6 A the comparator can never trip.

---

## 4. Pattern Execution

- Per-gun **sheet FIFO depth = 4** (`SHEET_QUEUE_DEPTH` in `PatternScheduler.cpp`).
- Sheet entries store only the encoder pulse count at the photocell leading
  edge. Patterns are walked **lazily** at runtime — dots are not pre-expanded.
- Pattern coordinate origin = paper leading edge =
  `photocell pulse + photocell_offset_mm * pulses_per_mm`.
- If gap between sheets < `photocell_offset_mm`, multiple sheets are tracked
  in flight on the same gun. Queue overflow drops the new trigger silently.
- Timing: the pattern task publishes each gun's next due pulse; the 50 us
  supervisor tick wakes it when the encoder reaches it (was: polled once per
  1 ms tick).  A line's end pulse is armed when it opens and the supervisor
  closes the gun there itself.  The task still runs every 1 ms for the speed
  gate and display.
- Speed-safety (lines only): if measured speed drops below `min_speed_mm_s`
  inside a line, the gun is closed immediately; it re-opens if speed recovers
  (>= 105 % of min, hysteresis) while still inside the line region.  Line
  closes always run.  Dots fire at any speed.  With `min_speed_mm_s` = 0 a
  line still closes when no encoder pulse arrived for 200 ms (conveyor
  stopped) and re-opens when it moves.
- A pattern **shape** change on a gun (type / elements) drops that gun's
  in-flight sheets and closes its line; it restarts from the next sheet.
  On-time-only changes keep running.  Stop / watchdog / fault / safety stop
  flush every in-flight sheet.
- Speed measurement (no per-pulse interrupts; PCNT count polled ~1 ms):
  - Displayed speed: 300 ms sliding window, sampled every 10 ms.
  - Min-speed gate: log of count changes with poll timestamps.  "Too slow"
    when no pulse arrived for longer than one pulse period at min speed, or
    when the upper speed bound over the last 30 ms is below min.  "Resume"
    only when the lower bound reaches 105 % of min.  In between, state holds.

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
- **Watchdog**: any received NDJSON command (incl. `ping`) and the web
  page's `/api/control/heartbeat` (sent every 500 ms while the page is
  visible) refresh the timestamp. After 2 s of silence from both while
  active, outputs are killed and `{"event":"watchdog_timeout"}` is emitted.
- **Safety supervisor**: see §3a.
- **Boot window**: from reset until `drv::init()` the ESP32 pins float.  IN1
  lines are held low by the DRV8262's internal 200 k pull-downs; IN2 comes
  from the MUX whose select / IN0 inputs have no pull-downs.  The DAC EEPROM
  value (§3b) covers the comparator side; a full fix needs pull-downs on the
  MUX_x_SELECT / MUX_x_IN0 lines (next board revision).

---

## 7. Protocol — Deltas From `initial prompt.md` §8

### 7.1 New error reason

| Where          | Reason                  | Meaning                                                 |
| -------------- | ----------------------- | ------------------------------------------------------- |
| `test_open`    | `no_pattern_or_busy`    | Gun has no pattern, or a test is already running on it. |
| `set_pattern`  | `bad_on_timeout`        | `on_timeout_ms` was supplied but ≤ 0.                   |
| `set_config`   | `pick_too_high`         | `pick_current_a` above 1.5 A.                           |
| `gun1`..`gun4`, `rt` | `pick_timeout`, `decay_timeout`, `close_late`, `in1_stray`, `tick_stalled`, `safety_stop` | Safety trips, see §3a. |
| `dac`          | `i2c_write_failed`, `eeprom_*_failed`, `init_failed` | DAC problems.                          |

All other validation errors (`bad_pulses_per_mm`, `hold_ge_pick`, etc.) are
sanity checks in `set_config` / `set_pattern`.

### 7.1a Shared control — PC and web together

There is no control owner any more: the PC (UART) and the phone (SoftAP web
page) may both change config, patterns and programs at any time, **including
while the machine is active** (a SPIFFS write can cause a brief timing
hiccup). `comms/LiveSync.{h,cpp}` keeps them consistent:

- One recursive edit lock serialises every writer of the config buffer and
  the program store (UART RX task, web handlers, autosave, calibration).
- A revision counter increments on every change. `/api/status` reports
  `rev` and `program_id`; the page reloads what it shows when `rev` moves,
  skipping a field the user is typing in until they leave it. Responses to
  changing requests carry `prev` / `rev` so the page ignores its own edits.
- Changes not made over UART are pushed to the PC as `config`, `pattern`
  and `programs_list` events. A rejected `set_config` / `set_pattern` from
  the PC is answered with the unchanged state so the GUI reverts.
- Calibration results are applied in task context (not from the ISR),
  autosaved, and broadcast like any other change.

New UART command: `{"cmd":"get_state"}` replies with one `config` event,
one `pattern` event per gun and a `programs_list` event. The GUI sends it on
connect and after `ready`, and never pushes its local defaults.

The UART line limit and the web command body limit are 4096 bytes, so a full
64-element pattern fits.

### 7.1b Sheet counter

- The firmware counts photocell leading edges accepted while active and not
  faulted.  Reported as `sheet_count` in the UART `status` event and in
  `GET /api/status`.
- `{"cmd":"reset_sheet_count"}` zeroes it (ack `reset_sheet_count`).  The
  GUI reset button sends this, so the count is shared by PC and web.

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
- **Lines mode**: `on_timeout_ms` is not used; a line opens with a 5 s
  ceiling (`LINE_CEILING_MS`) and is closed by encoder position at `end_mm`
  (supervisor tick, `seq::closeIfDrop`).  Speed-safety (`min_speed_mm_s`)
  applies to lines only: below it the line is paused (gun closed) and
  resumed when speed recovers inside the line region.
- **No "stuck in Peak"**: the on-time counts from `fire()`, and the 3 ms
  pick limit (§3a) closes a line whose hold threshold is not confirmed.

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
| Multi-program management | `ui/widgets/program_bar.py` — the program store lives on the controller (SPIFFS) and is shared with the web UI; dropdown, save, save-as, new, delete; edits autosave 2 s after the last change, also while running |
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
| Sync on connect | GUI pulls the controller state with `get_state` when the link opens; the controller is the source of truth |
| Resync on firmware reboot | GUI pulls the state again whenever a `ready` event arrives (the controller restores its active program from flash at boot) |
| Live sync with web UI | Web-side edits arrive as `config` / `pattern` / `programs_list` events and update the GUI immediately |
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

- **PCNT glitch filter** at 100 APB ticks (~1.25 µs). Will tune once we see the 6N137's real edge behaviour on the scope.
- **`NEAR_ZERO_V = 0.10 V`** Phase-3 termination threshold. Tunable in `rt/GunSequencer.cpp`. Right now corresponds to ~0.05 A coil current.
- **Peak diagnostics**: during `test_open` each drop reports a `debug` event
  `peak` (us to the LM339 trip) or `nopeak`.
- **TO CHECK — real-time / safety rework (2026-10-09, built, IRAM check
  clean, NOT yet run on the machine).**  Flash, then:
  1. Boot: no `error` events (`dac eeprom_*`, `rt timer_init_failed`); the
     first boot writes the DAC EEPROM once.
  2. Dots and lines run as before; drop size and position unchanged at low
     speed, more regular at high speed.  `status` `max_event_late_pulses`
     should stay ~0-1.
  3. No `pick_timeout` / `close_late` / `decay_timeout` during normal runs.
     A `pick_timeout` on lines means the coil needs more than 3 ms to reach
     pick (raise `PICK_LIMIT_US`) or the sense path is wrong.
  4. Edit a line's pattern while it runs: that gun stops for the current
     sheet and continues from the next one; no gun stays open.
  5. Stop the conveyor mid-line with min speed 0: the line closes after
     ~200 ms and re-opens when the conveyor moves.
  6. Test buttons still work; `peak` debug events show time-to-peak.
  7. Settings: pick above 1.5 A is refused.
- **TO CHECK — lines min-speed pause/resume (added 2026-10-06, flashed but
  NOT yet tested on the conveyor).**  See §4 speed-safety and
  `speedGate()` in `rt/PatternScheduler.cpp`.  Test in Lines mode:
  1. Slow below `min_speed_mm_s` mid-line -> gun must close immediately.
  2. Full stop mid-line -> gun closes within ~one pulse period (no blob).
  3. Speed back up while still inside the line -> gun re-opens and the line
     finishes at `end_mm`.
  4. Speed hovering at the limit -> no rapid on/off chatter.
  5. Dots unaffected by min speed; GUI/web speed reading smooth (300 ms).
  Note: `pulses_per_mm` was still 1 at the time -- calibrate first.

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
  comms/Events.{h,cpp}    UartJson.{h,cpp}   CommandDispatcher.{h,cpp}   LiveSync.{h,cpp}
  hw/Pins.h               Driver.{h,cpp}   Dac.{h,cpp}   Encoder.{h,cpp}   RtTimers.{h,cpp}
  rt/Control.{h,cpp}      GunSequencer.{h,cpp}   PatternScheduler.{h,cpp}   TestRunner.{h,cpp}
  sys/Fault.{h,cpp}       Watchdog.{h,cpp}       Status.{h,cpp}
tools/
  check_isr_iram.py            Interrupt flash-safety check on the built ELF
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
