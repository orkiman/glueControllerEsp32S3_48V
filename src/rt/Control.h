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

// Calibration: arm to count pulses between next two photocell edges of a sheet
// of known paper_length_mm; emits {"event":"calib_result"} when done.
void onCalibArm(float paper_length_mm);

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
