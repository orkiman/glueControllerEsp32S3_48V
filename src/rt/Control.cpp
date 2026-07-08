#include "Control.h"
#include "GunSequencer.h"
#include "PatternScheduler.h"
#include "TestRunner.h"
#include "hw/Driver.h"
#include "hw/Dac.h"
#include "hw/Encoder.h"
#include "hw/Pins.h"
#include "config/Config.h"
#include "comms/Events.h"

namespace rt {

void onSetActive(bool active) {
    if (active) {
        // Re-arm from a possibly inactive / faulted state.  Operator clears
        // the fault latch by issuing set_active:true (see UartJson).
        pattern::onConfigApplied();
        seq::onConfigApplied();
    } else {
        // Hard abort: flush sheet queues, stop tests, abort in-flight sequences.
        testrun::stopAll();
        pattern::abortAll();
        seq::abortAll();
        drv::killAll();
    }
}

void onConfigApplied() {
    pattern::onConfigApplied();
    seq::onConfigApplied();
}

void onCalibArm(float paperLengthMm) {
    pattern::onCalibArm(paperLengthMm);
}

void onTestOpen(uint8_t gun, uint32_t timeout_ms) {
    if (cfg::g_sys.fault.load(std::memory_order_acquire)) {
        evt::postError("test_open", "hardware_fault");
        return;
    }
    if (!cfg::g_sys.active.load(std::memory_order_acquire)) {
        evt::postError("test_open", "not_active");
        return;
    }
    // Behaviour follows the gun's configured pattern type:
    //   Lines -> single line held for timeout_ms (hardware-regulated).
    //   Dots  -> 20 Hz dot train until timeout_ms or test_close.
    if (gun > 0 && gun <= pins::NUM_GUNS) {
        if (cfg::Config::active()->pattern[gun - 1].type == cfg::PatternType::None) {
            evt::postError("test_open", "no_pattern");
            return;
        }
    }
    if (!testrun::start(gun, timeout_ms)) {
        evt::postError("test_open", "busy");
    }
}

void onTestClose(uint8_t gun) {
    testrun::stop(gun);
}

void onSwTrigger() {
    encoder::injectSwTrigger();
}

void emergencyShutdown() {
    drv::killAll();
    seq::abortAll();
    pattern::abortAll();
    dac::blockingZeroAll();
}

} // namespace rt
