#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "hw/Pins.h"

// =============================================================================
// Per-gun "Dot Sequence" state machine.
//
// Phase 1 (Peak):
//   - DAC[g] := pick threshold (e.g. 2.0V for 1.0A)
//   - IN1=HIGH, MUX_SELECT=HIGH (arm hardware loop)
//   - Enable peak interrupt for this gun
//
// On-timer:  armed by fire() at the START of Phase 1.  Counts the full
//   Peak+Hold budget (per-gun pattern.on_timeout_ms, or caller-supplied
//   onMs override).  When it expires we drop straight to Phase 3 no
//   matter which phase we are in -- this is also the fail-safe for
//   "peak trip never arrived".
//
// Phase 2 (Hold):  triggered by LM339 -> peakIsr
//   - Disable own peak interrupt (mask the chopping signals)
//   - DAC[g] := hold threshold (e.g. 0.8V for 0.4A)
//   - The on-timer keeps counting the remaining budget.
//
// Phase 3 (Active Fast Decay):  triggered by on-timer (or abort())
//   - DAC[g] := ~zero threshold (e.g. 0.1V)
//   - IN1=LOW   (MUX_SELECT stays HIGH so LM339 drives IN2 -> Reverse Drive)
//   - When current collapses past the near-zero threshold, LM339 drops IN2,
//     driver enters coast.  We then return to Idle.
// =============================================================================

namespace seq {

enum class Phase : uint8_t {
    Idle    = 0,
    Peak    = 1,
    Hold    = 2,
    Decay   = 3,
};

void init();                                                       // wires up peak ISRs + gptimers

// Fire a full dot sequence on gun g (0..3).
// onMs == 0 => use the gun's pattern.on_timeout_ms; otherwise override
// with a caller-supplied total Peak+Hold budget in milliseconds.
// Returns false if the gun is busy or system is faulted / inactive.
bool fire(uint8_t gunIdx, uint32_t onMs = 0) IRAM_ATTR;

// Abort any in-flight sequence on gun g (immediate Phase 3).
// gun == 0xFF -> all guns.  IRAM-safe.
void abort(uint8_t gunIdx) IRAM_ATTR;
void abortAll() IRAM_ATTR;

Phase phaseOf(uint8_t gunIdx);                                     // diagnostics
bool  isBusy(uint8_t gunIdx);

// Called by Config when set_config publishes a new threshold table.
void onConfigApplied();

// Enable/disable per-gun peak-detection diagnostics.  When on, each fire()
// emits a `debug` event on peak trip ("peak" + us since fire) or, if the
// on-timer expires without a trip, a "nopeak" event.  TestRunner turns this
// on only for the duration of a manual test so normal production stays quiet.
void setDiag(bool on);

} // namespace seq
