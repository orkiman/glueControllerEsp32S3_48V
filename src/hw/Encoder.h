#pragma once
#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// Dual encoder counter using the ESP32 PCNT peripheral so the CPU never sees
// a per-pulse interrupt:
//   - Primary: fast 5V encoder on GPIO40 (pins::ENCODER), via 6N137.
//   - Alternate: 24V opto encoder on GPIO5 (pins::ENCODER_ALT).
// Both are counted in parallel on independent PCNT units at all times.
// pulseCount() reports whichever one cfg::RuntimeConfig::encoder_source
// selects -- switching is a pure config change, no reflash/rewiring needed.
//
// Also owns the photocell input ISR (GPIO 1 via TLP291), with software
// debounce honouring cfg::RuntimeConfig::debounce_ms.  On a valid edge it
// calls pattern::onPhotocellEdge(pulseCountAtEdge) directly (Core-1 ISR ctx).
// =============================================================================

namespace encoder {

void init();                                                // PCNT + photocell ISR

// Returns the current monotonic pulse count.  IRAM-safe.
uint32_t pulseCount() IRAM_ATTR;

// Software-injected photocell edge (sw_trigger command).  Task context.
void injectSwTrigger();

} // namespace encoder
