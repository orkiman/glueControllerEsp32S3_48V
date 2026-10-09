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
//   * ISRs and RT code never touch I2C.  They set a per-channel target code
//     (setTarget) and wake the DacTask (kick).
//   * DacTask runs on Core 1 and writes every channel whose target differs
//     from what is on the chip, in one I2C transaction.
//   * isApplied() tells whether a code is physically on the chip, so the
//     sequencer can confirm a threshold before relying on it.
// =============================================================================

namespace dac {

// 0.10 V: with this threshold the comparators read "no current" when the coil
// is off.  Used as the power-on / shutdown value.
constexpr uint16_t SAFE_CODE = 124;

bool init();                                                  // I2C + chip + DacTask

// Set the target code of one channel.  Any context, IRAM-safe; takes effect
// after kick().
void setTarget(uint8_t gunIdx, uint16_t code) IRAM_ATTR;

// Wake the DacTask to write pending targets.  Task or ISR context.
void kick() IRAM_ATTR;

// True if `code` is both the target and confirmed written to the chip.
bool isApplied(uint8_t gunIdx, uint16_t code) IRAM_ATTR;

// Convert a threshold voltage (0..VDD) to its 12-bit DAC code.  Uses float
// math, so call it from task context and cache the result for ISR use.
uint16_t codeForVolts(float volts);

// Convenience: convert amps -> volts using INA240 transfer (Vout = 2*I).
inline float ampsToVolts(float a) { return a * 2.0f; }

// TASK CONTEXT ONLY: set one channel and write it synchronously (blocking
// I2C).  Returns true once the code is on the chip.
bool blockingSetCode(uint8_t gunIdx, uint16_t code);

// TASK CONTEXT ONLY: write SAFE_CODE to every channel (shutdown path).
void blockingSafeAll();

} // namespace dac
