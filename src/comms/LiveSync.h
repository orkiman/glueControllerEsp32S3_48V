#pragma once
#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// Shared-state synchronisation between the two HMIs (PC over UART, phone over
// the SoftAP web page).  Both may edit config, patterns and programs at any
// time, including while the machine is running.
//
//   * One recursive edit lock serialises every writer of the config double
//     buffer and the program store (UART RX task, network task, autosave).
//   * A revision counter increments on every change.  The web page polls it
//     via /api/status and reloads when it moves.
//   * Changes are pushed to the PC as config / pattern / programs_list events.
//     A set_config / set_pattern that came *from* the PC is not echoed back.
// =============================================================================

namespace livesync {

enum class Source : uint8_t { Uart = 0, Web = 1, Internal = 2 };

void init();                       // call before any task can edit

void lock();
void unlock();

class Guard {
public:
    Guard()  { lock(); }
    ~Guard() { unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

uint32_t revision();

// Notifications (call with the lock held, after the change is published).
void configChanged(Source src);
void patternChanged(uint8_t gun_1based, Source src);
void programsChanged();            // list / names / active id changed
void programLoaded();              // whole config replaced from storage

// Send the full current state (config, all patterns, program list) over UART.
void emitFullState();
void emitProgramsList();

// ISR-safe: hand a calibration result to task context for applying.
void IRAM_ATTR postCalibration(float pulses_per_mm);

// Task context, call often: applies a pending calibration result.
void service();

} // namespace livesync
