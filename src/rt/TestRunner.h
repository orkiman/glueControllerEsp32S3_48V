#pragma once
#include <stdint.h>

// =============================================================================
// Diagnostic test runner.
//
// test_open behaviour (per gun) depends on the gun's currently configured
// pattern type:
//
//   Lines : open the gun as a single line for `timeout_ms`, then auto-close.
//           test_close aborts early -> immediate Phase 3.
//
//   Dots  : fire individual dots at 20 Hz until either `timeout_ms` elapses
//           or test_close is received. Each dot uses pattern[g].on_timeout_ms.
//
//   None  : error (no pattern configured for this gun).
//
// For test_open with gun==0 the same logic is broadcast to all 4 guns.
// Tests run only while the machine is stopped (not active, not calibrating);
// starting the machine or calibration stops them.
// =============================================================================

namespace testrun {

void init();

// Returns false if the gun cannot start a test (busy, no pattern, fault, etc.).
// On gun==0 starts a test on every gun that has a pattern configured.
bool start(uint8_t gunOneBased, uint32_t timeout_ms);

// Stop a running test (and abort any in-flight gun sequence).
void stop(uint8_t gunOneBased);
void stopAll();

// True while a test is running on any gun.
bool anyRunning();

} // namespace testrun
