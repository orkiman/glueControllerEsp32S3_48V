#include "GunSequencer.h"
#include "hw/Driver.h"
#include "hw/Dac.h"
#include "hw/RtTimers.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace seq {

// Near-zero threshold for the decay (volts).  0.1 V on the INA240 == 0.05 A:
// the LM339 drops IN2 as the coil current collapses past it, ending the
// reverse drive before any negative current flows.
static constexpr float    NEAR_ZERO_V          = 0.10f;

static constexpr int64_t  NEVER                = INT64_MAX;
static constexpr int64_t  MIN_ON_US            = 50;
static constexpr int64_t  MAX_ON_US            = 10'000'000;
static constexpr int64_t  ALARM_SLACK_US       = 2;      // close alarm runs on its own 1 MHz clock
static constexpr int64_t  SUPERVISOR_GRACE_US  = 100;    // supervisor closes what the alarm missed
static constexpr int64_t  CHECKER_GRACE_US     = 2000;   // Core 0 last resort
static constexpr int64_t  DECAY_STUCK_US       = 2000;   // near-zero on the DAC, current still up
static constexpr int64_t  DECAY_MAX_US         = 5000;   // near-zero never confirmed: let the coil coast
static constexpr uint8_t  DECAY_LOW_TICKS      = 2;      // comparator low this many ticks in a row
static constexpr uint32_t CHECKER_PERIOD_US    = 1000;
static constexpr uint8_t  TICK_STALL_CHECKS    = 3;
// A trip streak ends with a good drop (hold confirmed) or after this long
// without a trip, so isolated trips far apart never add up to a stop.
static constexpr int64_t  STREAK_RESET_US      = 60'000'000;

// ---------- per-gun state, guarded by s_mux ----------
// IN1 is high exactly while phase != Idle: fire() and closeLocked() change
// both together under the lock.
struct GunRt {
    volatile Phase    phase        = Phase::Idle;
    volatile bool     arming       = false;  // fire() is writing the pick threshold
    volatile bool     holdOk       = false;  // hold threshold confirmed on the DAC this drop
    volatile bool     decaying     = false;  // closed, LM339 still reverse-driving to ~0 A
    volatile bool     nearZeroSeen = false;
    volatile bool     peakSeen     = false;
    volatile uint8_t  lowTicks     = 0;
    volatile uint8_t  tripStreak   = 0;      // safety trips since the last good drop
    volatile uint32_t drop         = 0;      // id of the current / last drop
    volatile int64_t  fireUs       = 0;
    volatile int64_t  closeAtUs    = NEVER;
    volatile int64_t  decayStartUs = 0;
    volatile int64_t  nearZeroAtUs = 0;
    volatile int64_t  lastTripUs   = 0;
    volatile uint32_t peakDtUs     = 0;
    // Peak diagnostics handed to the Core 0 checker (task context).
    volatile bool     diagPending  = false;
    volatile bool     diagPeak     = false;
    volatile uint32_t diagUs       = 0;
    // DAC codes (precomputed in task context: no FPU in interrupts).
    volatile uint16_t cPick        = 0;
    volatile uint16_t cHold        = 0;
    volatile uint16_t cNearZ       = 0;
};

static GunRt              s_g[pins::NUM_GUNS];
static portMUX_TYPE       s_mux        = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_diag       = false;
static volatile bool      s_rtOk       = false;   // hardware timers running
static TickHook           s_tickHook   = nullptr;
static volatile uint32_t  s_tickCount  = 0;
static uint32_t           s_ticksSeen  = 0;       // Core 0 checker only
static uint8_t            s_tickStalls = 0;       // Core 0 checker only
static esp_timer_handle_t s_checker    = nullptr;

static volatile uint32_t   s_tripCount      = 0;
static volatile uint8_t    s_lastTripGun    = 0;
static volatile TripReason s_lastTripReason = TripReason::None;

// Event text in DRAM: interrupts read it while the flash cache may be off.
static_assert(pins::NUM_GUNS == 4, "kGunName lists four guns");
static DRAM_ATTR const char kGunName[pins::NUM_GUNS + 1][6] = { "rt", "gun1", "gun2", "gun3", "gun4" };
static DRAM_ATTR const char kReasonName[][16] = {
    "", "pick_timeout", "decay_timeout", "close_late", "in1_stray", "tick_stalled", "safety_stop",
};

// Trips found while holding the lock; posted after it is released.
struct Notes {
    struct Note { uint8_t gun1; TripReason reason; };   // gun1: 1-based, 0 = not gun specific
    Note    n[2 * pins::NUM_GUNS + 2];
    uint8_t count   = 0;
    bool    kickDac = false;
};

// ---------- helpers ----------
static inline bool IRAM_ATTR systemArmed() {
    return cfg::g_sys.active.load(std::memory_order_acquire) &&
          !cfg::g_sys.fault .load(std::memory_order_acquire);
}

static inline void IRAM_ATTR copyStr(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static inline bool IRAM_ATTR pinHigh(uint32_t inLo, uint32_t inHi, int8_t pin) {
    return pin < 32 ? ((inLo >> pin) & 1u) : ((inHi >> (pin - 32)) & 1u);
}

// gpio_intr_enable/disable live in flash; these mirror them with inline LL
// calls and also clear a pending edge.
static inline void IRAM_ATTR peakIntrDisable(uint8_t g) {
    gpio_num_t pin = (gpio_num_t)pins::PEAK_IRQ[g];
    gpio_ll_intr_disable(&GPIO, pin);
    if (pin < 32) gpio_ll_clear_intr_status(&GPIO, BIT(pin));
    else          gpio_ll_clear_intr_status_high(&GPIO, BIT(pin - 32));
}

static inline void IRAM_ATTR peakIntrEnable(uint8_t g) {
    gpio_num_t pin = (gpio_num_t)pins::PEAK_IRQ[g];
    if (pin < 32) gpio_ll_clear_intr_status(&GPIO, BIT(pin));
    else          gpio_ll_clear_intr_status_high(&GPIO, BIT(pin - 32));
    gpio_ll_intr_enable_on_core(&GPIO, 1, pin);
}

// Program the close alarm for the earliest open drop.
static void IRAM_ATTR rearmCloseAlarmLocked(int64_t nowUs) {
    int64_t next = NEVER;
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        if (s_g[g].phase != Phase::Idle && s_g[g].closeAtUs < next) next = s_g[g].closeAtUs;
    }
    if (next == NEVER) { rttimer::closeAlarmOff(); return; }
    int64_t d = next - nowUs;
    if (d < 0) d = 0;
    rttimer::closeAlarmIn((uint32_t)d);      // d <= MAX_ON_US
}

// Close gun g: IN1 low (always), start the decay.  Returns true if a DAC
// write was requested (caller kicks the DAC task after releasing the lock).
static bool IRAM_ATTR closeLocked(uint8_t g, int64_t nowUs) {
    GunRt& s = s_g[g];
    peakIntrDisable(g);
    drv::setIn1(g, false);
    s.arming = false;                        // cancels a fire() still writing the DAC
    if (s.phase == Phase::Idle) return false;
    if (s_diag && !s.diagPending) {
        s.diagPeak    = s.peakSeen;
        s.diagUs      = s.peakSeen ? s.peakDtUs : (uint32_t)(nowUs - s.fireUs);
        s.diagPending = true;
    }
    s.phase        = Phase::Idle;
    s.closeAtUs    = NEVER;
    // MUX_SELECT stays HIGH: the LM339 reverse-drives the coil down to ~0 A.
    s.decaying     = true;
    s.nearZeroSeen = false;
    s.lowTicks     = 0;
    s.decayStartUs = nowUs;
    dac::setTarget(g, s.cNearZ);
    return true;
}

// Decay done (or given up): IN2 back under ESP32 control, which holds it low,
// and the pick threshold goes onto the DAC now, so the next fire() does not
// have to wait for an I2C write.  Returns true: the caller kicks the DAC.
static bool IRAM_ATTR endDecayLocked(uint8_t g) {
    drv::setMuxSelect(g, false);
    s_g[g].decaying = false;
    dac::setTarget(g, s_g[g].cPick);
    return true;
}

static void IRAM_ATTR noteLocked(Notes& notes, uint8_t gun1, TripReason r) {
    s_tripCount      = s_tripCount + 1;
    s_lastTripGun    = gun1;
    s_lastTripReason = r;
    if (notes.count < sizeof(notes.n) / sizeof(notes.n[0])) {
        notes.n[notes.count].gun1   = gun1;
        notes.n[notes.count].reason = r;
        notes.count++;
    }
}

// Machine stop after repeated trips: same end state as a stop command.
static void IRAM_ATTR stopAllLocked(int64_t nowUs, Notes& notes) {
    cfg::g_sys.active.store(false, std::memory_order_release);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) notes.kickDac |= closeLocked(g, nowUs);
    drv::killAll();                          // IN1, IN2, MUX_SELECT low: coast
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_g[g].decaying = false;
    rearmCloseAlarmLocked(nowUs);
}

static void IRAM_ATTR tripLocked(uint8_t g, TripReason r, int64_t nowUs, Notes& notes) {
    noteLocked(notes, g + 1, r);
    GunRt& s = s_g[g];
    if (nowUs - s.lastTripUs > STREAK_RESET_US) s.tripStreak = 0;
    s.lastTripUs = nowUs;
    if (s.tripStreak < 255) s.tripStreak = s.tripStreak + 1;
    if (s.tripStreak >= TRIP_STREAK_STOP && cfg::g_sys.active.load(std::memory_order_acquire)) {
        stopAllLocked(nowUs, notes);
        noteLocked(notes, g + 1, TripReason::SafetyStop);
    }
}

static void IRAM_ATTR fillTripEvent(evt::Event& e, const Notes::Note& n) {
    e.kind = evt::Kind::Error;
    copyStr(e.cmd,    sizeof(e.cmd),    kGunName[n.gun1 <= pins::NUM_GUNS ? n.gun1 : 0]);
    copyStr(e.reason, sizeof(e.reason), kReasonName[(uint8_t)n.reason]);
}

static void IRAM_ATTR postNotesFromIsr(const Notes& notes, BaseType_t* hp) {
    for (uint8_t i = 0; i < notes.count; ++i) {
        evt::Event e{};
        fillTripEvent(e, notes.n[i]);
        evt::postFromISR(e, hp);
    }
}

static void postNotesFromTask(const Notes& notes) {
    for (uint8_t i = 0; i < notes.count; ++i) {
        evt::Event e{};
        fillTripEvent(e, notes.n[i]);
        evt::post(e);
    }
}

// ---------- interrupts ----------
static void IRAM_ATTR peakIsr(void* arg) {
    uint8_t g = (uint8_t)(uintptr_t)arg;
    bool kick = false;
    portENTER_CRITICAL_ISR(&s_mux);
    peakIntrDisable(g);                      // mask the chopping edges that follow
    GunRt& s = s_g[g];
    if (s.phase == Phase::Peak) {
        s.phase    = Phase::Hold;
        s.peakSeen = true;
        s.peakDtUs = (uint32_t)(esp_timer_get_time() - s.fireUs);
        dac::setTarget(g, s.cHold);          // LM339 chops at hold from here
        kick = true;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    if (kick) dac::kick();
}

static void IRAM_ATTR onCloseAlarm() {
    int64_t now = esp_timer_get_time();
    bool kick = false;
    portENTER_CRITICAL_ISR(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        if (s_g[g].phase != Phase::Idle && s_g[g].closeAtUs <= now + ALARM_SLACK_US) {
            kick |= closeLocked(g, now);
        }
    }
    rearmCloseAlarmLocked(now);
    portEXIT_CRITICAL_ISR(&s_mux);
    if (kick) dac::kick();
}

static void IRAM_ATTR superviseDecayLocked(uint8_t g, int64_t now, bool cmpHigh, Notes& notes) {
    GunRt& s = s_g[g];
    bool nearZero = dac::isApplied(g, s.cNearZ);
    if (nearZero && !s.nearZeroSeen) { s.nearZeroSeen = true; s.nearZeroAtUs = now; }
    if (nearZero && !cmpHigh) {
        if (s.lowTicks < 255) s.lowTicks = s.lowTicks + 1;
        if (s.lowTicks >= DECAY_LOW_TICKS) notes.kickDac |= endDecayLocked(g);
        return;
    }
    s.lowTicks = 0;
    if (nearZero && now - s.nearZeroAtUs >= DECAY_STUCK_US) {
        notes.kickDac |= endDecayLocked(g);  // current (or the sense) stuck high
        tripLocked(g, TripReason::DecayTimeout, now, notes);
    } else if (now - s.decayStartUs >= DECAY_MAX_US) {
        notes.kickDac |= endDecayLocked(g);  // DAC write late: coast finishes the decay
    }
}

// IN1 output latch.  Read per check: a trip may have just switched pins.
static inline bool IRAM_ATTR in1High(uint8_t g) {
    static_assert(pins::DRV_IN1[0] < 32 && pins::DRV_IN1[1] < 32 &&
                  pins::DRV_IN1[2] < 32 && pins::DRV_IN1[3] < 32, "IN1 pins read from GPIO.out");
    return (GPIO.out >> pins::DRV_IN1[g]) & 1u;
}

static void IRAM_ATTR superviseLocked(int64_t now, Notes& notes) {
    uint32_t inLo = GPIO.in;
    uint32_t inHi = GPIO.in1.val;
    bool closed = false;
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        GunRt& s = s_g[g];
        if (s.phase != Phase::Idle) {
            if (now - s.closeAtUs >= SUPERVISOR_GRACE_US) {
                notes.kickDac |= closeLocked(g, now);
                closed = true;
                tripLocked(g, TripReason::CloseLate, now, notes);
                continue;
            }
            if (!s.holdOk) {
                if (s.phase == Phase::Hold && dac::isApplied(g, s.cHold)) {
                    s.holdOk     = true;
                    s.tripStreak = 0;
                } else if (now - s.fireUs >= (int64_t)PICK_LIMIT_US) {
                    notes.kickDac |= closeLocked(g, now);
                    closed = true;
                    tripLocked(g, TripReason::PickTimeout, now, notes);
                }
            }
            continue;
        }
        if (in1High(g)) {
            drv::setIn1(g, false);
            tripLocked(g, TripReason::In1Stray, now, notes);
        }
        if (s.decaying) superviseDecayLocked(g, now, pinHigh(inLo, inHi, pins::PEAK_IRQ[g]), notes);
    }
    if (closed) rearmCloseAlarmLocked(now);
}

static void IRAM_ATTR onSupervisorTick() {
    s_tickCount = s_tickCount + 1;
    int64_t now = esp_timer_get_time();
    Notes notes;
    portENTER_CRITICAL_ISR(&s_mux);
    superviseLocked(now, notes);
    portEXIT_CRITICAL_ISR(&s_mux);

    BaseType_t hp = pdFALSE;
    postNotesFromIsr(notes, &hp);
    if (notes.kickDac) dac::kick();
    if (s_tickHook) s_tickHook(&hp);
    if (hp) portYIELD_FROM_ISR();
}

// ---------- Core 0 checker (esp_timer task) ----------
static void emitDiag() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        bool     pending;
        bool     peak;
        uint32_t us;
        portENTER_CRITICAL(&s_mux);
        pending = s_g[g].diagPending;
        peak    = s_g[g].diagPeak;
        us      = s_g[g].diagUs;
        s_g[g].diagPending = false;
        portEXIT_CRITICAL(&s_mux);
        if (!pending) continue;
        // "peak": us from open to the LM339 trip.  "nopeak": the drop ended
        // first (open coil, dead sense, threshold not reached, short on-time).
        evt::Event e{};
        e.kind = evt::Kind::Debug;
        copyStr(e.cmd, sizeof(e.cmd), peak ? "peak" : "nopeak");
        e.b1 = (uint8_t)(g + 1);
        e.f1 = (float)us;
        evt::post(e);
    }
}

static void checkerCb(void*) {
    int64_t now = esp_timer_get_time();
    Notes notes;
    portENTER_CRITICAL(&s_mux);
    bool closed = false;
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        GunRt& s = s_g[g];
        if (s.phase != Phase::Idle) {
            if (now - s.closeAtUs >= CHECKER_GRACE_US) {
                notes.kickDac |= closeLocked(g, now);
                closed = true;
                tripLocked(g, TripReason::CloseLate, now, notes);
            }
        } else if (in1High(g)) {
            drv::setIn1(g, false);
            tripLocked(g, TripReason::In1Stray, now, notes);
        }
    }
    if (closed) rearmCloseAlarmLocked(now);
    // The supervisor tick must keep counting; restart it if it stopped.
    uint32_t ticks = s_tickCount;
    if (ticks != s_ticksSeen) {
        s_ticksSeen  = ticks;
        s_tickStalls = 0;
    } else if (++s_tickStalls >= TICK_STALL_CHECKS) {
        s_tickStalls = 0;
        rttimer::supervisorRestart();
        noteLocked(notes, 0, TripReason::TickStalled);
    }
    portEXIT_CRITICAL(&s_mux);

    postNotesFromTask(notes);
    if (notes.kickDac) dac::kick();
    emitDiag();
}

// ---------- init ----------
static void initPeakPin(uint8_t g) {
    gpio_config_t c = {};
    c.pin_bit_mask = (1ULL << pins::PEAK_IRQ[g]);
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;     // ext 10k pull-up to 3V3
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_POSEDGE;
    gpio_config(&c);
    gpio_isr_handler_add((gpio_num_t)pins::PEAK_IRQ[g], peakIsr, (void*)(uintptr_t)g);
    peakIntrDisable(g);                       // armed only while a drop is in Peak
}

void init() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) initPeakPin(g);
    onConfigApplied();

    // The timer interrupts are bound to this core; setup() runs on Core 1.
    s_rtOk = rttimer::init(&onCloseAlarm, &onSupervisorTick);
    if (!s_rtOk)               evt::postError("rt", "timer_init_failed");
    if (xPortGetCoreID() != 1) evt::postError("rt", "rt_not_on_core1");

    esp_timer_create_args_t a = {};
    a.callback        = &checkerCb;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name            = "rt_check";
    esp_timer_create(&a, &s_checker);
    esp_timer_start_periodic(s_checker, CHECKER_PERIOD_US);
}

void onConfigApplied() {
    const cfg::RuntimeConfig* c = cfg::Config::active();
    // FPU work here, in task context; interrupts only use the codes.
    uint16_t pick = dac::codeForVolts(dac::ampsToVolts(c->pick_current_a));
    uint16_t hold = dac::codeForVolts(dac::ampsToVolts(c->hold_current_a));
    uint16_t nz   = dac::codeForVolts(NEAR_ZERO_V);
    portENTER_CRITICAL(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        GunRt& s = s_g[g];
        s.cPick  = pick;
        s.cHold  = hold;
        s.cNearZ = nz;
        // Idle guns get the (new) pick threshold now; a decaying gun gets it
        // when its decay ends.
        if (s.phase == Phase::Idle && !s.arming && !s.decaying) dac::setTarget(g, pick);
    }
    portEXIT_CRITICAL(&s_mux);
    dac::kick();
}

void onActivate() {
    portENTER_CRITICAL(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_g[g].tripStreak = 0;
    portEXIT_CRITICAL(&s_mux);
}

void setTickHook(TickHook hook) { s_tickHook = hook; }
void setDiag(bool on)           { s_diag = on; }

// ---------- public API ----------
bool fire(uint8_t g, uint32_t onMs, uint32_t* outDrop) {
    if (g >= pins::NUM_GUNS || xPortInIsrContext()) return false;
    if (!s_rtOk || !systemArmed()) return false;

    int64_t onUs = (onMs == 0)
        ? (int64_t)(cfg::Config::active()->pattern[g].on_timeout_ms * 1000.0f)
        : (int64_t)onMs * 1000;
    if (onUs < MIN_ON_US) onUs = MIN_ON_US;
    if (onUs > MAX_ON_US) onUs = MAX_ON_US;

    GunRt& s = s_g[g];
    portENTER_CRITICAL(&s_mux);
    bool busy = s.phase != Phase::Idle || s.arming;   // still open: ignore this drop
    if (!busy) s.arming = true;
    uint16_t pick = s.cPick;
    portEXIT_CRITICAL(&s_mux);
    if (busy) return false;

    // The pick threshold must be on the DAC output before IN1 goes high, or
    // the LM339 trips against the old threshold and regulates at hold.
    // Normally it was set when the previous drop's decay ended; otherwise
    // (drop right after a close, config change) write it now.
    bool dacOk = dac::isApplied(g, pick) || dac::blockingSetCode(g, pick);

    bool     opened = false;
    uint32_t drop   = 0;
    portENTER_CRITICAL(&s_mux);
    if (s.arming && dacOk && systemArmed()) {
        int64_t now = esp_timer_get_time();
        s.arming    = false;
        s.drop      = (s.drop + 1 == 0) ? 1 : s.drop + 1;
        s.fireUs    = now;
        s.closeAtUs = now + onUs;
        s.holdOk    = false;
        s.peakSeen  = false;
        s.decaying  = false;
        s.phase     = Phase::Peak;
        rearmCloseAlarmLocked(now);           // the close is armed before IN1 goes high
        drv::setMuxSelect(g, true);           // LM339 drives IN2
        peakIntrEnable(g);
        drv::setIn1(g, true);
        drop   = s.drop;
        opened = true;
    } else {
        s.arming = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (opened && outDrop) *outDrop = drop;
    return opened;
}

void IRAM_ATTR abort(uint8_t g) {
    if (g >= pins::NUM_GUNS) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_mux);
    bool kick = closeLocked(g, now);
    rearmCloseAlarmLocked(now);
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (kick) dac::kick();
}

void IRAM_ATTR abortAll() {
    int64_t now = esp_timer_get_time();
    bool kick = false;
    portENTER_CRITICAL_SAFE(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) kick |= closeLocked(g, now);
    rearmCloseAlarmLocked(now);
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (kick) dac::kick();
}

void IRAM_ATTR closeIfDrop(uint8_t g, uint32_t drop) {
    if (g >= pins::NUM_GUNS || drop == 0) return;
    int64_t now = esp_timer_get_time();
    bool kick = false;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_g[g].drop == drop && s_g[g].phase != Phase::Idle) {
        kick = closeLocked(g, now);
        rearmCloseAlarmLocked(now);
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (kick) dac::kick();
}

Phase phaseOf(uint8_t g) {
    return (g < pins::NUM_GUNS) ? s_g[g].phase : Phase::Idle;
}

bool isBusy(uint8_t g) {
    return phaseOf(g) != Phase::Idle;
}

TripInfo tripInfo() {
    TripInfo t;
    portENTER_CRITICAL(&s_mux);
    t.count  = s_tripCount;
    t.gun    = s_lastTripGun;
    t.reason = s_lastTripReason;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

const char* tripReasonName(TripReason r) {
    uint8_t i = (uint8_t)r;
    return i < sizeof(kReasonName) / sizeof(kReasonName[0]) ? kReasonName[i] : "";
}

} // namespace seq
