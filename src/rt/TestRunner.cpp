#include "TestRunner.h"
#include "GunSequencer.h"
#include "hw/Pins.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>

namespace testrun {

static constexpr uint32_t DOT_RATE_HZ      = 20;
static constexpr uint64_t DOT_PERIOD_US    = 1'000'000ull / DOT_RATE_HZ;

enum class Mode : uint8_t { None = 0, Line = 1, Dots = 2 };

struct GunTest {
    std::atomic<Mode>   mode{Mode::None};
    esp_timer_handle_t  tickTimer = nullptr;   // periodic, used in Dots mode
    esp_timer_handle_t  deadlineTimer = nullptr;
    uint32_t            deadlineMs = 0;        // for diagnostics
};

static GunTest s_t[pins::NUM_GUNS];

// ----- forward decls -----
static void doStop(uint8_t g);

// ----- timer callbacks -----
static void dotTickCb(void* user) {
    uint8_t g = (uint8_t)(uintptr_t)user;
    if (s_t[g].mode.load(std::memory_order_acquire) != Mode::Dots) return;
    seq::fire(g, 0);   // on-time = pattern[g].on_timeout_ms
}

static void deadlineCb(void* user) {
    uint8_t g = (uint8_t)(uintptr_t)user;
    doStop(g);
}

// ----- helpers -----
static void ensureTimers(uint8_t g) {
    if (!s_t[g].tickTimer) {
        esp_timer_create_args_t a = {};
        a.callback        = &dotTickCb;
        a.arg             = (void*)(uintptr_t)g;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name            = "test_tick";
        esp_timer_create(&a, &s_t[g].tickTimer);
    }
    if (!s_t[g].deadlineTimer) {
        esp_timer_create_args_t a = {};
        a.callback        = &deadlineCb;
        a.arg             = (void*)(uintptr_t)g;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name            = "test_dl";
        esp_timer_create(&a, &s_t[g].deadlineTimer);
    }
}

static void doStop(uint8_t g) {
    if (g >= pins::NUM_GUNS) return;
    Mode prev = s_t[g].mode.exchange(Mode::None, std::memory_order_acq_rel);
    if (prev == Mode::None) return;

    if (s_t[g].tickTimer)     esp_timer_stop(s_t[g].tickTimer);
    if (s_t[g].deadlineTimer) esp_timer_stop(s_t[g].deadlineTimer);
    seq::abort(g);
}

static bool startOne(uint8_t g, uint32_t timeout_ms) {
    if (g >= pins::NUM_GUNS) return false;
    if (cfg::g_sys.fault.load(std::memory_order_acquire)) return false;

    cfg::PatternType pt = cfg::Config::active()->pattern[g].type;
    if (pt == cfg::PatternType::None) return false;

    // Cancel any previous test on this gun before starting a new one.
    doStop(g);
    ensureTimers(g);

    s_t[g].deadlineMs = millis() + timeout_ms;

    if (pt == cfg::PatternType::Lines) {
        // Single open-for-timeout line.  seq::fire's hold cap is enforced
        // inside the sequencer (10 s); command validation enforces timeout<=10000.
        if (!seq::fire(g, timeout_ms)) return false;
        s_t[g].mode.store(Mode::Line, std::memory_order_release);
        // Deadline timer not strictly needed (hold timer already drives Phase 3
        // at timeout), but we use it to clear our `mode` state for stop()/diag.
        esp_timer_start_once(s_t[g].deadlineTimer, (uint64_t)timeout_ms * 1000ull);
        return true;
    }

    // Dots: periodic 20 Hz tick + one-shot deadline.
    s_t[g].mode.store(Mode::Dots, std::memory_order_release);
    seq::fire(g, 0);                                              // first dot now
    esp_timer_start_periodic(s_t[g].tickTimer, DOT_PERIOD_US);
    esp_timer_start_once    (s_t[g].deadlineTimer, (uint64_t)timeout_ms * 1000ull);
    return true;
}

// ----- public API -----
void init() {
    // Lazy timer creation in ensureTimers(); nothing to do here.
}

bool start(uint8_t gunOneBased, uint32_t timeout_ms) {
    // Peak diagnostics are only interesting during a manual test; leave them
    // off during production pattern firing to keep the event stream quiet.
    seq::setDiag(true);
    if (gunOneBased == 0) {
        bool any = false;
        for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
            if (cfg::Config::active()->pattern[g].type != cfg::PatternType::None) {
                any = startOne(g, timeout_ms) || any;
            }
        }
        return any;
    }
    if (gunOneBased > pins::NUM_GUNS) return false;
    return startOne(gunOneBased - 1, timeout_ms);
}

void stop(uint8_t gunOneBased) {
    if (gunOneBased == 0) { stopAll(); return; }
    if (gunOneBased > pins::NUM_GUNS) return;
    doStop(gunOneBased - 1);
}

void stopAll() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) doStop(g);
    seq::setDiag(false);
}

} // namespace testrun
