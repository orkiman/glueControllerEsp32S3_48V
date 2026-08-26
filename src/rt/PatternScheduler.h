#pragma once
#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// Pattern scheduler (Core 1).
//
// Responsibilities:
//   - Maintain a FIFO of in-flight sheets (depth 4) per gun.  Each sheet
//     stores the encoder pulse count at its leading-edge photocell event.
//   - Walk each sheet's pattern lazily (no pre-expansion of dot lists).
//   - Fire events when the encoder reaches their absolute pulse positions:
//       * Dot:  one seq::fire(g) per dot, on-time budget = pattern[g].on_timeout_ms.
//       * Line: seq::fire(g, BIG) at start, seq::abort(g) at end (position-driven).
//   - Provide calibration mode: count pulses between the leading and trailing
//     photocell edges of one sheet of known length, then publish pulses_per_mm.
//
// Per-gun-per-sheet events are naturally monotonic in pulse count, so no heap
// is needed: we just inspect each gun's head (oldest) sheet instance and pop
// it once all its elements have been processed.
// =============================================================================

namespace pattern {

void init();                                                       // creates PatternTask

// Called from photocell ISR (IRAM ctx) on every valid rising edge.
void onPhotocellEdge(uint32_t pulseCountAtEdge) IRAM_ATTR;
// Called from photocell ISR on falling edge (used only by calibration).
void onPhotocellFallingEdge(uint32_t pulseCountAtEdge) IRAM_ATTR;

// Calibration arm (task ctx, from rt::onCalibArm).
void onCalibArm(float paperLengthMm);

// Abort everything: flush sheet queues, abort all in-flight gun sequences.
void abortAll();

// Reload cached scalars from the active config (pulses_per_mm, offset, ...).
void onConfigApplied();

// Diagnostics for status events.
struct Metrics {
    uint32_t max_loop_gap_us;
    uint32_t max_event_late_pulses;
    uint32_t pattern_events;
    uint32_t sheet_queue_overflows;
};

float currentPosMm();
float currentSpeedMmS();
Metrics metrics();

} // namespace pattern
