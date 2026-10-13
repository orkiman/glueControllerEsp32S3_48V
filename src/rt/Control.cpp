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
        // Gun tests run only while stopped: starting the machine ends them.
        testrun::stopAll();
        // Re-arm from a possibly inactive / faulted state.  Operator clears
        // the fault latch by issuing set_active:true (see UartJson).
        pattern::onConfigApplied();
        seq::onConfigApplied();
        seq::onActivate();
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

static bool  s_calibResume = false;     // machine was active when armed
static float s_calibResult = -1.0f;

static void leaveCalibration(bool resume) {
    cfg::g_sys.calibrating.store(false, std::memory_order_release);
    if (resume && s_calibResume && !cfg::g_sys.fault.load(std::memory_order_acquire)) {
        onSetActive(true);
        cfg::g_sys.active.store(true, std::memory_order_release);
    }
}

void onCalibArm(float paperLengthMm) {
    testrun::stopAll();                     // no gun tests while measuring
    if (!cfg::g_sys.calibrating.load(std::memory_order_acquire)) {
        // Same stop as set_active:false -- nothing fires while measuring.
        s_calibResume = cfg::g_sys.active.load(std::memory_order_acquire);
        if (s_calibResume) {
            onSetActive(false);
            cfg::g_sys.active.store(false, std::memory_order_release);
        }
        cfg::g_sys.calibrating.store(true, std::memory_order_release);
    }
    s_calibResult = -1.0f;
    pattern::onCalibArm(paperLengthMm);    // re-arm restarts the measurement
}

void onCalibCancel(bool resume) {
    if (!cfg::g_sys.calibrating.load(std::memory_order_acquire)) return;
    pattern::onCalibCancel();
    leaveCalibration(resume);
}

void onCalibDone(float pulsesPerMm) {
    if (!cfg::g_sys.calibrating.load(std::memory_order_acquire)) return;
    s_calibResult = pulsesPerMm > 0.0f ? pulsesPerMm : 0.0f;
    leaveCalibration(true);
}

float lastCalibResult() { return s_calibResult; }

void onTestOpen(uint8_t gun, uint32_t timeout_ms) {
    if (cfg::g_sys.fault.load(std::memory_order_acquire)) {
        evt::postError("test_open", "hardware_fault");
        return;
    }
    // Gun tests run only while the machine is stopped.
    if (cfg::g_sys.active.load(std::memory_order_acquire)) {
        evt::postError("test_open", "machine_active");
        return;
    }
    if (cfg::g_sys.calibrating.load(std::memory_order_acquire)) {
        evt::postError("test_open", "calibrating");
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

void onResetSheetCount() {
    pattern::resetSheetCount();
}

void emergencyShutdown() {
    drv::killAll();
    seq::abortAll();
    pattern::abortAll();
    dac::blockingSafeAll();
}

} // namespace rt
