#pragma once
#include <Arduino.h>

// =============================================================================
// Hardware timers for the real-time layer.  Both count at 1 MHz and raise
// IRAM interrupts on the core that called init() (Core 1), so they keep
// running while Core 0 writes flash and do not depend on any task:
//
//   Close alarm  (TIMERG0 timer 0): one-shot, armed by the gun sequencer for
//                the earliest drop end.
//   Supervisor   (TIMERG1 timer 0): fires every SUPERVISOR_PERIOD_US.  Runs
//                the safety checks and the encoder due-pulse check.
//
// Separate timer groups and separate interrupts, so a fault in one does not
// stop the other: the supervisor backs up the close alarm.
// =============================================================================

namespace rttimer {

constexpr uint32_t SUPERVISOR_PERIOD_US = 50;

using IsrFn = void (*)();

// Allocate both interrupts on the calling core and start the supervisor.
// The callbacks run in interrupt context and must be IRAM_ATTR.
bool init(IsrFn onCloseAlarm, IsrFn onSupervisorTick);

// Fire the close alarm `delayUs` from now (at least a few us).  IRAM-safe.
void closeAlarmIn(uint32_t delayUs) IRAM_ATTR;
void closeAlarmOff() IRAM_ATTR;

// Re-arm the supervisor tick (recovery if it ever stops).  IRAM-safe.
void supervisorRestart() IRAM_ATTR;

} // namespace rttimer
