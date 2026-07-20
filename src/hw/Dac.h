#pragma once
#include <Arduino.h>
#include <atomic>
#include "Pins.h"

// =============================================================================
// MCP4728 12-bit quad DAC (I2C @ 400 kHz).
//
// Sets the dynamic threshold voltage for each gun's LM339 comparator:
//   Vthresh = I_amps * 2     (INA240 gain 50 V/V * 40 mOhm shunt)
//
// Channel mapping: DAC channel N -> Gun (N+1).
//
// Hot-path strategy:
//   * ISRs and RT tasks NEVER touch I2C. They call requestThreshold(g, V),
//     which atomically updates a shadow and pings the DacTask.
//   * DacTask runs on Core 1, drains pending requests, and pushes them to the
//     chip via Adafruit_MCP4728::fastWrite (single I2C transaction, ~225 us
//     at 400 kHz for all 4 channels).
//   * If multiple requests arrive while a write is in flight, only the LATEST
//     shadow value is sent (lossless from the hardware-correctness viewpoint:
//     the comparator only cares about the final threshold).
// =============================================================================

namespace dac {

bool init();                                                  // I2C + chip + DacTask

// Request a new threshold (in volts, 0..VDD) for one gun.  Returns immediately.
// TASK CONTEXT ONLY: performs floating-point math (volts->code).  Calling this
// from an ISR triggers a Coprocessor exception (the ESP32 FPU is disabled in
// interrupt context).  From ISR paths, precompute the code with codeForVolts()
// in task context and call requestCode() instead.
void requestThreshold(uint8_t gunIdx, float volts);

// Request a new raw 12-bit DAC code for one gun.  Pure integer path, so it is
// safe from ANY context including ISR.  Returns immediately.
void requestCode(uint8_t gunIdx, uint16_t code) IRAM_ATTR;

// Convert a threshold voltage (0..VDD) to its 12-bit DAC code.  Uses float
// math, so call it from task context (e.g. when config changes) and cache the
// result for ISR-time use via requestCode().
uint16_t codeForVolts(float volts);

// Convenience: convert amps -> volts using INA240 transfer (Vout = 2*I).
inline float ampsToVolts(float a) { return a * 2.0f; }

// TASK CONTEXT ONLY: set one channel's raw code and push it to the chip
// synchronously (blocking I2C, ~225 us).  Use this from fire() to guarantee the
// pick threshold is physically present on the DAC output *before* the coil is
// energised, so the LM339 does not trip against the stale threshold from the
// previous cycle and regulate at hold current immediately.
void blockingSetCode(uint8_t gunIdx, uint16_t code);

// Force-write everything to a single low value (used on emergencyShutdown).
// Blocks briefly on I2C; call from task context only.
void blockingZeroAll();

} // namespace dac
