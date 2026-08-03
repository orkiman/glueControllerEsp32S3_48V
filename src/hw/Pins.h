#pragma once
#include <stdint.h>

// =============================================================================
// Cold Glue Controller - ESP32-S3 Pin Map
// Source of truth: docs/initial prompt.md  Section 4
// =============================================================================

namespace pins {

constexpr uint8_t NUM_GUNS = 4;

// OPTO_IN: four general-purpose 24V opto inputs (GPIO1, 2, 4, 5).  These are
// NOT per-gun signals -- despite sharing the array shape with the per-gun pin
// tables below, they are independent, general-purpose inputs.  Current /
// planned usage in THIS project:
//   OPTO_IN[0] (GPIO1) -- PHOTOCELL: material-arrival trigger, shared by ALL
//                         guns (drives pattern::onPhotocellEdge/-FallingEdge
//                         for the whole pattern scheduler, not just "gun 1").
//   OPTO_IN[1] (GPIO2) -- unused / spare.
//   OPTO_IN[2] (GPIO4) -- unused / spare.
//   OPTO_IN[3] (GPIO5) -- ALTERNATIVE encoder input: a 24V opto-isolated
//                         encoder option, as opposed to the fast 5V encoder
//                         wired to GPIO40 (see ENCODER_ALT below).  Counted
//                         in parallel with the primary encoder on its own
//                         PCNT unit; cfg::RuntimeConfig::encoder_source picks
//                         which one actually drives pattern position tracking.
constexpr int8_t OPTO_IN[NUM_GUNS]    = { 1,  2,  4,  5  };

// Alternative 24V opto encoder input, same physical shaft as ENCODER (GPIO40).
constexpr int8_t ENCODER_ALT = OPTO_IN[3];

// Per-gun pin arrays, indexed 0..3 (Gun 1..Gun 4).
constexpr int8_t DRV_IN1[NUM_GUNS]    = { 6,  7, 15, 16  }; // DRV8262 IN1/IN3 main drive
constexpr int8_t MUX_IN2[NUM_GUNS]    = { 8,  9, 17, 18  }; // MUX I0 input (manual ESP32 override)
constexpr int8_t MUX_SELECT[NUM_GUNS] = {10, 11, 14, 48  }; // S=1 LM339 control, S=0 ESP32 control
constexpr int8_t PEAK_IRQ[NUM_GUNS]   = {12, 13, 21, 38  }; // From LM339, ext 10k pull-up

// Photocell / material-arrival trigger, shared by all guns (see OPTO_IN[0] above).
constexpr int8_t PHOTOCELL = OPTO_IN[0];

// --- Global pins ---
constexpr int8_t I2C_SDA  = 41;   // -> MCP4728
constexpr int8_t I2C_SCL  = 42;   // -> MCP4728
constexpr int8_t N_FAULT  = 39;   // DRV8262 nFAULT, active LOW, ext 10k pull-up
constexpr int8_t ENCODER  = 40;   // 6N137 single-channel pulse input (fast, 5V).
                                   // Alternative 24V option is ENCODER_ALT (GPIO5, above).

// --- UART0 (HMI link via CP2102 / pin header) ---
// Default Arduino Serial uses these on ESP32-S3 DevKitC-1.
constexpr int8_t UART_TX  = 43;
constexpr int8_t UART_RX  = 44;

} // namespace pins
