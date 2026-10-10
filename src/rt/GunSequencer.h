#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "hw/Pins.h"

// =============================================================================
// Per-gun drop sequence and its safety net.
//
// Phase 1 (Peak):  fire()
//   - DAC[g] := pick threshold (must be on the chip before IN1 goes high)
//   - MUX_SELECT=HIGH (LM339 drives IN2), peak interrupt armed, IN1=HIGH
//   - The drop end ("close") is armed BEFORE IN1 goes high.
//
// Phase 2 (Hold):  LM339 trip -> peakIsr
//   - DAC[g] := hold threshold; the LM339 keeps chopping at hold.
//
// Close (Phase 3): at the on-time end, a line end, abort() or a safety trip
//   - IN1=LOW, DAC[g] := near-zero.  MUX_SELECT stays HIGH so the LM339
//     reverse-drives the coil down to ~0 A ("decay").
//   - Once the near-zero threshold is on the DAC and the comparator reads
//     low (coil current ~0), MUX_SELECT goes LOW: between drops IN2 is held
//     low by the ESP32, so nothing on the comparator side can drive the coil.
//     The pick threshold then goes onto the DAC, so the next fire() normally
//     needs no I2C write.
//
// Ways a drop is closed (independent of each other and of every task):
//   1. Close alarm: hardware timer interrupt at the exact on-time end.
//   2. Supervisor tick (every 50 us, interrupt): closes any drop past its end.
//   3. Core 0 checker (esp_timer, 1 ms): last resort, 2 ms after the end.
// The supervisor also enforces:
//   - Pick limit: the hold threshold must be confirmed on the DAC within
//     PICK_LIMIT_US of opening, else the gun closes (missed peak trip, dead
//     current sense, failed or late DAC write).
//   - IN1 readback: an IN1 pin that is high without an open drop is forced low.
//   - Decay limit: if the coil current does not fall to ~0 after closing,
//     MUX_SELECT is forced low.
// Every safety trip closes the gun and is reported as
//   {"event":"error","cmd":"gunN","reason":...}.  TRIP_STREAK_STOP trips in a
// row on one gun (no good drop in between, less than 60 s apart) stop the
// machine: {"event":"error","cmd":"gunN","reason":"safety_stop"}.
// =============================================================================

namespace seq {

enum class Phase : uint8_t {
    Idle = 0,     // IN1 low
    Peak = 1,     // IN1 high, regulating at pick
    Hold = 2,     // IN1 high, regulating at hold
};

constexpr uint32_t PICK_LIMIT_US    = 3000;
constexpr uint8_t  TRIP_STREAK_STOP = 3;

enum class TripReason : uint8_t {
    None = 0,
    PickTimeout,     // hold threshold not confirmed within PICK_LIMIT_US
    DecayTimeout,    // current did not fall to ~0 after closing
    CloseLate,       // a drop was still open after its end (close alarm missed)
    In1Stray,        // IN1 high without an open drop
    TickStalled,     // supervisor tick had stopped and was restarted
    SafetyStop,      // machine stopped after repeated trips
};

void init();                                  // peak ISRs, hardware timers, checker

// Open one drop on gun g (0..3).  TASK CONTEXT ONLY: if the pick threshold is
// not already on the DAC it is written synchronously over I2C first.
// onMs == 0 => the gun's pattern.on_timeout_ms; otherwise the caller's total
// on-time in ms (lines: long ceiling, closed by position).  Returns false if
// the gun is still open (the new drop is ignored), the DAC write failed or
// the system is inactive / faulted.  On success *outDrop receives the drop id.
bool fire(uint8_t gunIdx, uint32_t onMs = 0, uint32_t* outDrop = nullptr);

// Close the gun now.  Any context, IRAM-safe.
void abort(uint8_t gunIdx) IRAM_ATTR;
void abortAll() IRAM_ATTR;
// Close only if `drop` is still the open drop on that gun.  IRAM-safe.
void closeIfDrop(uint8_t gunIdx, uint32_t drop) IRAM_ATTR;

Phase phaseOf(uint8_t gunIdx);
bool  isBusy(uint8_t gunIdx);

// Refresh DAC codes after a config change.
void onConfigApplied();
// Machine (re)started: clear the trip streaks.
void onActivate();

// Extra work for the supervisor tick (the pattern scheduler's encoder check).
// Runs in interrupt context; must be IRAM_ATTR.
using TickHook = void (*)(BaseType_t* higherPrioWoken);
void setTickHook(TickHook hook);

// Safety trip counters for status reporting.
struct TripInfo {
    uint32_t   count;      // trips since boot
    uint8_t    gun;        // 1-based gun of the last trip (0 = none)
    TripReason reason;
};
TripInfo tripInfo();
const char* tripReasonName(TripReason r);

// Supervisor tick count since boot (~20000 per second while it runs).
uint32_t supervisorTicks();

// Peak-detection diagnostics for manual tests: each drop reports a `debug`
// event "peak" (us from open to the LM339 trip) or "nopeak".
void setDiag(bool on);

} // namespace seq
