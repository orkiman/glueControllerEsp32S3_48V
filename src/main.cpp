#include <Arduino.h>

#include "config/Config.h"
#include "comms/Events.h"
#include "comms/UartJson.h"
#include "storage/ProgramStore.h"
#include "hw/Driver.h"
#include "hw/Dac.h"
#include "hw/Encoder.h"
#include "rt/GunSequencer.h"
#include "rt/PatternScheduler.h"
#include "rt/TestRunner.h"
#include "sys/Fault.h"
#include "sys/Watchdog.h"
#include "sys/Status.h"
#include "net/SoftAP.h"

// =============================================================================
// Cold Glue Controller - Main Controller firmware entry point.
//
// Core 0 (Management / Comms):
//   uart_rx   prio 4    NDJSON parser + dispatcher
//   evt_emit  prio 5    Drains evt:: queue -> Serial
//   wdog      prio 3    HMI link watchdog
//   status    prio 2    5 Hz status events
//
// Core 1 (Real-Time Hardware):
//   pattern   prio 7    Encoder poll + fire pattern events
//   dac       prio 6    MCP4728 I2C writer
//   <ISRs>    IRAM      Peak (per gun) + photocell + nFAULT + PCNT overflow
// =============================================================================

void setup() {
    // ---- 1. Config defaults (both buffers seeded) ----
    cfg::Config::init();

    // ---- 2. Safe-state every output BEFORE anything else can fire ----
    drv::init();

    // ---- 2a. Persistent program store: mount and restore last active snapshot ----
    prog::init();

    // ---- 3. Comms backbone: Serial + event emitter + RX/JSON ----
    evt::init();
    uartjson::init();

    // ---- 4. HW peripherals ----
    dac::init();          // I2C + DacTask (Core 1)
    encoder::init();      // PCNT + photocell ISR (any-edge)
    fault::init();        // nFAULT ISR

    // ---- 5. RT layer ----
    seq::init();          // peak ISRs + hold timers
    pattern::init();      // PatternTask (Core 1)
    testrun::init();      // diagnostic test runner

    // Notify program store that RT caches can now be refreshed from config.
    prog::onRuntimeInitialized();

    // ---- 6. Supervisors ----
    watchdog::init();
    status::init();
    net::init();

    // ---- 7. Announce readiness to HMI ----
    evt::postReady();
}

void loop() {
    // Everything runs in FreeRTOS tasks.  The Arduino loopTask sits idle.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
