#include "GunSequencer.h"
#include "hw/Driver.h"
#include "hw/Dac.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace seq {

// Tunable: the Phase-3 "near zero" threshold (volts).
// 0.1 V on the INA240 == 0.05 A coil current — small enough that the LM339
// drops IN2 the moment the coil current collapses past it, terminating the
// reverse drive before any negative current flows.
static constexpr float NEAR_ZERO_V = 0.10f;

// ---------- per-gun runtime state ----------
struct GunRt {
    std::atomic<Phase>  phase{Phase::Idle};
    // The on-timer counts the full Peak+Hold budget from the start of
    // seq::fire().  When it fires we drop into Phase 3 regardless of which
    // phase we're in -- which also guards against "stuck in Peak" when the
    // LM339 trip never arrives (open coil, broken sense, etc.).
    esp_timer_handle_t  onTimer = nullptr;
    // Cached thresholds (volts) refreshed from cfg on every config publish.
    float               vPick   = 2.0f;
    float               vHold   = 0.8f;
    float               vNearZ  = NEAR_ZERO_V;
    // Same thresholds pre-converted to raw 12-bit DAC codes.  ISR paths MUST
    // use these (via dac::requestCode) -- converting volts->code needs the FPU,
    // which is disabled in interrupt context (Coprocessor exception -> reboot).
    uint16_t            cPick   = 0;
    uint16_t            cHold   = 0;
    uint16_t            cNearZ  = 0;
    // Diagnostics: when did this fire() start, and did a peak trip arrive?
    volatile int64_t    fireUs  = 0;
    volatile bool       peakSeen = false;
    volatile uint32_t   peakDtUs = 0;     // time-to-peak (us), integer only
};

static GunRt s_g[pins::NUM_GUNS];

// When true, fire() reports peak-trip timing per gun via `debug` events.
static std::atomic<bool> s_diag{false};

void setDiag(bool on) { s_diag.store(on, std::memory_order_release); }

// ---------- helpers ----------
static inline bool systemArmed() {
    return cfg::g_sys.active.load(std::memory_order_acquire) &&
          !cfg::g_sys.fault .load(std::memory_order_acquire);
}

// ---------- Phase 3 entry ----------
static void IRAM_ATTR enterPhase3(uint8_t g) {
    s_g[g].phase.store(Phase::Decay, std::memory_order_release);
    dac::requestCode(g, s_g[g].cNearZ);   // ISR-safe (no FPU); reachable via faultIsr
    drv::setIn1(g, false);            // MUX_SELECT stays HIGH -> reverse drive
    // Hardware completes the active fast decay autonomously.  Mark idle.
    s_g[g].phase.store(Phase::Idle, std::memory_order_release);
}

// ---------- on-timer callback (esp_timer task ctx) ----------
// Fires when the per-gun on-timeout elapses, whether we are still in Peak
// (peak trip never came) or already in Hold (normal end-of-droplet).
static void onTimerCb(void* user) {
    uint8_t g = (uint8_t)(uintptr_t)user;
    // If a peak trip is still pending we must mask the IRQ; otherwise a
    // late LM339 edge could re-enter peakIsr after we already dropped to
    // Phase 3 below.
    gpio_intr_disable((gpio_num_t)pins::PEAK_IRQ[g]);
    if (s_diag.load(std::memory_order_acquire)) {
        // Report the outcome of this fire cycle.  Runs in esp_timer TASK
        // context, so floating-point (e.f1) is safe here -- unlike peakIsr,
        // which only records integer state.
        //   "peak"   -> LM339 signalled peak; e.f1 = time-to-peak (us)
        //   "nopeak" -> on-timer expired first (open coil / dead comparator /
        //               miswired sense / threshold never reached)
        evt::Event e{}; e.kind = evt::Kind::Debug;
        const char* tag = s_g[g].peakSeen ? "peak" : "nopeak";
        strncpy(e.cmd, tag, sizeof(e.cmd) - 1);
        e.b1 = (uint8_t)(g + 1);
        e.f1 = s_g[g].peakSeen
                   ? (float)s_g[g].peakDtUs
                   : (float)(esp_timer_get_time() - s_g[g].fireUs);
        evt::post(e);
    }
    enterPhase3(g);
}

// ---------- peak ISR (LM339 rising edge per gun) ----------
static void IRAM_ATTR peakIsr(void* arg) {
    uint8_t g = (uint8_t)(uintptr_t)arg;
    if (s_g[g].phase.load(std::memory_order_acquire) != Phase::Peak) return;

    // Mask own IRQ first thing -- avoid being flooded by chopping signals.
    gpio_intr_disable((gpio_num_t)pins::PEAK_IRQ[g]);

    s_g[g].phase.store(Phase::Hold, std::memory_order_release);

    // Update DAC to hold threshold; LM339 will autonomously chop from here.
    // MUST use the integer requestCode() path: this is ISR context and the
    // FPU is disabled here (a float volts->code conversion caused the
    // "Coprocessor exception" reboot).  No timer action -- the on-timer armed
    // by fire() keeps counting the remaining on-time budget.
    dac::requestCode(g, s_g[g].cHold);

    // Diagnostics: record integer state only (no FPU in ISR).  onTimerCb emits
    // the "peak"/"nopeak" event later in task context where float is allowed.
    s_g[g].peakDtUs = (uint32_t)(esp_timer_get_time() - s_g[g].fireUs);
    s_g[g].peakSeen = true;
}

// ---------- init helpers ----------
static void initPeakPin(uint8_t g) {
    gpio_config_t c = {};
    c.pin_bit_mask = (1ULL << pins::PEAK_IRQ[g]);
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;     // ext 10k pull-up to 3V3
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_POSEDGE;
    gpio_config(&c);
    gpio_isr_handler_add((gpio_num_t)pins::PEAK_IRQ[g],
                         peakIsr, (void*)(uintptr_t)g);
    gpio_intr_disable((gpio_num_t)pins::PEAK_IRQ[g]);   // armed only during fire()
}

static void initOnTimer(uint8_t g) {
    esp_timer_create_args_t a = {};
    a.callback        = &onTimerCb;
    a.arg             = (void*)(uintptr_t)g;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name            = "on";
    esp_timer_create(&a, &s_g[g].onTimer);
}

void init() {
    // GPIO ISR service was installed by encoder::init().  If not, install now.
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        initPeakPin(g);
        initOnTimer(g);
    }
    onConfigApplied();
}

void onConfigApplied() {
    const cfg::RuntimeConfig* c = cfg::Config::active();
    float vPick = dac::ampsToVolts(c->pick_current_a);
    float vHold = dac::ampsToVolts(c->hold_current_a);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        s_g[g].vPick  = vPick;
        s_g[g].vHold  = vHold;
        s_g[g].vNearZ = NEAR_ZERO_V;
        // Pre-convert to raw codes here (task context, FPU available) so the
        // ISR paths never touch floating point.
        s_g[g].cPick  = dac::codeForVolts(vPick);
        s_g[g].cHold  = dac::codeForVolts(vHold);
        s_g[g].cNearZ = dac::codeForVolts(NEAR_ZERO_V);
    }
}

// ---------- public API ----------
bool IRAM_ATTR fire(uint8_t g, uint32_t onMs) {
    if (g >= pins::NUM_GUNS) return false;
    if (!systemArmed())      return false;

    // CAS to Peak; if not currently Idle the gun is busy.
    Phase expected = Phase::Idle;
    if (!s_g[g].phase.compare_exchange_strong(expected, Phase::Peak,
                                              std::memory_order_acq_rel)) {
        return false;
    }

    // On-timeout = total Peak+Hold budget, measured from RIGHT NOW.
    // onMs == 0 means "use the per-gun configured on_timeout_ms (Dots mode)";
    // any non-zero caller value (lines = long ceiling, tests) overrides.
    uint64_t onUs = (onMs == 0)
        ? (uint64_t)(cfg::Config::active()->pattern[g].on_timeout_ms * 1000.0f)
        : (uint64_t)onMs * 1000ull;
    if (onUs < 50)        onUs = 50;          // 50 us minimum sanity
    if (onUs > 5'000'000) onUs = 5'000'000;   // 5 s hard ceiling

    // Diagnostics: stamp the start and clear the peak-seen flag so onTimerCb
    // can tell whether the LM339 ever signalled peak for this gun.
    s_g[g].fireUs   = esp_timer_get_time();
    s_g[g].peakSeen = false;

    // Phase 1: arm DAC to pick, drive IN1, route LM339 to IN2, enable peak IRQ.
    //
    // The pick threshold MUST be physically on the DAC output *before* we drive
    // IN1 and arm the peak IRQ.  The async requestCode() path only queues a
    // shadow write for dacTask, which lands ~225 us later; by then the coil is
    // already ramping against the STALE threshold left by the previous cycle
    // (cNearZ ~0.10 V, or 0 on the very first shot).  The LM339 trips against
    // that low reference almost immediately, peakIsr switches to hold, and the
    // gun regulates at HOLD current from the outset -- so the pick-current
    // setting appears to do nothing on the scope.  fire() always runs in task
    // context (esp_timer TASK callbacks / pattern task), so a blocking I2C write
    // is safe here; guard against the IRAM/ISR case just in case.
    if (!xPortInIsrContext()) {
        dac::blockingSetCode(g, s_g[g].cPick);   // synchronous: present before drive
    } else {
        dac::requestCode(g, s_g[g].cPick);
    }
    drv::setMuxSelect(g, true);     // S=1 -> LM339 drives IN2
    drv::setIn1     (g, true);
    gpio_intr_enable((gpio_num_t)pins::PEAK_IRQ[g]);
    // Arm the full-cycle on-timer NOW so that "stuck in Peak" cannot last
    // longer than `onUs` even if the LM339 peak trip never arrives.  A
    // previous abort from ISR context (faultIsr) may have left a stale timer
    // armed -- stop it first so the start cannot fail with INVALID_STATE.
    // fire() always runs in task context, so esp_timer_stop is safe here.
    esp_timer_stop(s_g[g].onTimer);
    esp_timer_start_once(s_g[g].onTimer, onUs);
    return true;
}

void IRAM_ATTR abort(uint8_t g) {
    if (g >= pins::NUM_GUNS) return;
    Phase p = s_g[g].phase.load(std::memory_order_acquire);
    if (p == Phase::Idle) return;

    gpio_intr_disable((gpio_num_t)pins::PEAK_IRQ[g]);
    // esp_timer_stop() is NOT safe to call from an ISR (faultIsr -> abortAll
    // runs in interrupt context and would panic/reboot).  When called from a
    // task we stop the timer immediately; in ISR context we leave it armed --
    // it is one-shot and onTimerCb only re-enters Phase 3, which is harmless
    // once we are already Idle, and fire() stops any stale timer before reuse.
    if (!xPortInIsrContext()) {
        esp_timer_stop(s_g[g].onTimer);
    }
    enterPhase3(g);
}

void IRAM_ATTR abortAll() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) abort(g);
}

Phase phaseOf(uint8_t g) {
    return (g < pins::NUM_GUNS) ? s_g[g].phase.load() : Phase::Idle;
}

bool isBusy(uint8_t g) {
    return phaseOf(g) != Phase::Idle;
}

} // namespace seq
