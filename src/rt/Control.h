#pragma once
#include <Arduino.h>

// =============================================================================
// Glue control surface used by the JSON command dispatcher.
// All functions are safe to call from Core 0 (task context).
// Implementations live in the RT layer (PatternScheduler / GunSequencer).
// =============================================================================

namespace rt {

// Master enable. false => abort everything (in-flight Phase 3, flush queues).
void onSetActive(bool active);

// Apply currently published config to runtime caches (DAC threshold table, etc.).
void onConfigApplied();

// Calibration mode.  Arm: remember active/inactive, stop gluing (machine made
// inactive), count pulses from the photocell's rising to falling edge of one
// sheet of known paper_length_mm.  Done: apply pulses_per_mm, emit
// calib_result (or error calib_arm/no_pulses), restore the previous state.
// All Core 0, under livesync::Guard.
void onCalibArm(float paper_length_mm);
// Leave calibration mode without a result.  resume: restore the state from
// before arming (calib_cancel); false when a set_active overrides it.
void onCalibCancel(bool resume);
// Measurement finished (livesync::service).  pulses_per_mm 0 = no pulses.
void onCalibDone(float pulses_per_mm);
// For status: > 0 last result, 0 last attempt counted no pulses, < 0 none /
// cancelled since the last arm.
float lastCalibResult();

// Manual single-shot fire of the full dot sequence for diagnostics.
// gun: 1..4, or 0 = all guns simultaneously.
// timeout_ms acts as the hold-phase duration (NEVER raw IN1 HIGH).
void onTestOpen(uint8_t gun, uint32_t timeout_ms);

// Abort a test_open early -> immediate Phase 3.
void onTestClose(uint8_t gun);

// Software-injected photocell trigger.
void onSwTrigger();

// Zero the sheet counter reported in status (shared by PC and web).
void onResetSheetCount();

// Hard kill from fault path: drop everything, do not emit (caller emits).
void emergencyShutdown();

} // namespace rt
