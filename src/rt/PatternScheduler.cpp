#include "PatternScheduler.h"
#include "GunSequencer.h"
#include "hw/Encoder.h"
#include "hw/Pins.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

namespace pattern {

// ---------------- per-sheet, per-gun runtime instance ----------------
struct SheetGunInstance {
    uint32_t edgePulse;       // encoder pulse count at photocell leading edge
    uint16_t elemIdx;         // current element index within pattern
    uint16_t dotIdx;          // dot index within current dots-element
    bool     lineOpen;        // currently mid-line (line mode)
};

static constexpr uint8_t SHEET_QUEUE_DEPTH = 4;

struct GunQueue {
    SheetGunInstance ring[SHEET_QUEUE_DEPTH];
    uint8_t head = 0;         // index of oldest sheet
    uint8_t size = 0;
};

static GunQueue              s_q[pins::NUM_GUNS];
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- cached config scalars ----------------
static std::atomic<float>    s_pulsesPerMm{12.34f};
static std::atomic<float>    s_offsetMm{250.0f};

// ---------------- calibration state ----------------
enum class CalibState : uint8_t { Idle = 0, ArmedLeading = 1, ArmedTrailing = 2 };
static std::atomic<CalibState> s_calib{CalibState::Idle};
static std::atomic<float>      s_calibPaperLen{0.0f};
static std::atomic<uint32_t>   s_calibLeadPulse{0};

// ---------------- speed estimation ----------------
static std::atomic<float>     s_lastSpeedMmS{0.0f};
static std::atomic<uint32_t>  s_maxLoopGapUs{0};
static std::atomic<uint32_t>  s_maxEventLatePulses{0};
static std::atomic<uint32_t>  s_patternEvents{0};
static std::atomic<uint32_t>  s_sheetQueueOverflows{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

// ---------------- helpers ----------------
static inline void updateMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

static inline float pulsesToMm(uint32_t p) {
    float ppm = s_pulsesPerMm.load(std::memory_order_acquire);
    return (ppm > 0.0f) ? ((float)p / ppm) : 0.0f;
}

static inline uint32_t mmToPulses(float mm) {
    float ppm = s_pulsesPerMm.load(std::memory_order_acquire);
    float p = mm * ppm;
    if (p < 0.0f) p = 0.0f;
    return (uint32_t)(p + 0.5f);
}

// Compute the absolute pulse count of the *next* event for one sheet on one gun.
// Returns false if the sheet has no remaining events on this gun (instance done).
static bool nextEventPulse(uint8_t g, const SheetGunInstance& s,
                           uint32_t& outPulse, uint8_t& outAction)
{
    const cfg::GunPattern& gp = cfg::Config::active()->pattern[g];
    if (s.elemIdx >= gp.count || gp.type == cfg::PatternType::None) return false;

    const cfg::PatternElement& e = gp.elems[s.elemIdx];
    float ppm     = s_pulsesPerMm.load(std::memory_order_acquire);
    float offset  = s_offsetMm.load   (std::memory_order_acquire);
    uint32_t base = s.edgePulse + (uint32_t)((offset) * ppm + 0.5f);

    if (gp.type == cfg::PatternType::Lines) {
        if (!s.lineOpen) {
            outPulse  = base + (uint32_t)(e.start_mm * ppm + 0.5f);
            outAction = 1;    // open
        } else {
            outPulse  = base + (uint32_t)(e.end_mm * ppm + 0.5f);
            outAction = 2;    // close
        }
        return true;
    }
    // Dots
    float dotMm = e.start_mm + (float)s.dotIdx * e.spacing_mm;
    if (dotMm > e.end_mm + 0.0001f) return false;     // shouldn't happen, defensive
    outPulse  = base + (uint32_t)(dotMm * ppm + 0.5f);
    outAction = 0;
    return true;
}

// Advance instance past the action that just fired.
static void advanceInstance(uint8_t g, SheetGunInstance& s, uint8_t action) {
    const cfg::GunPattern& gp = cfg::Config::active()->pattern[g];
    if (s.elemIdx >= gp.count) return;
    const cfg::PatternElement& e = gp.elems[s.elemIdx];

    if (gp.type == cfg::PatternType::Lines) {
        if (action == 1) {           // just opened
            s.lineOpen = true;
        } else {                     // just closed -> next element
            s.lineOpen = false;
            s.elemIdx++;
        }
    } else { // Dots
        s.dotIdx++;
        float nextMm = e.start_mm + (float)s.dotIdx * e.spacing_mm;
        if (nextMm > e.end_mm + 0.0001f) {
            s.dotIdx  = 0;
            s.elemIdx++;
        }
    }
}

static bool instanceDone(uint8_t g, const SheetGunInstance& s) {
    const cfg::GunPattern& gp = cfg::Config::active()->pattern[g];
    return s.elemIdx >= gp.count || gp.type == cfg::PatternType::None;
}

// ---------------- ISR-context photocell hook ----------------
void IRAM_ATTR onPhotocellEdge(uint32_t pulseAtEdge) {
    // Calibration takes priority over normal triggering.
    CalibState cs = s_calib.load(std::memory_order_acquire);
    if (cs == CalibState::ArmedLeading) {
        s_calibLeadPulse.store(pulseAtEdge, std::memory_order_release);
        s_calib.store(CalibState::ArmedTrailing, std::memory_order_release);
        return;
    }

    if (!cfg::g_sys.active.load(std::memory_order_acquire)) return;
    if ( cfg::g_sys.fault .load(std::memory_order_acquire)) return;

    portENTER_CRITICAL_ISR(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        GunQueue& q = s_q[g];
        if (q.size >= SHEET_QUEUE_DEPTH) {
            s_sheetQueueOverflows.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        uint8_t slot = (q.head + q.size) % SHEET_QUEUE_DEPTH;
        q.ring[slot] = SheetGunInstance{ pulseAtEdge, 0, 0, false };
        q.size++;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

void IRAM_ATTR onPhotocellFallingEdge(uint32_t pulseAtEdge) {
    if (s_calib.load(std::memory_order_acquire) != CalibState::ArmedTrailing) return;

    uint32_t lead = s_calibLeadPulse.load(std::memory_order_acquire);
    float    L    = s_calibPaperLen .load(std::memory_order_acquire);
    if (L <= 0.0f) { s_calib.store(CalibState::Idle); return; }

    float ppm = (float)(pulseAtEdge - lead) / L;
    s_calib.store(CalibState::Idle, std::memory_order_release);

    // Publish new pulses_per_mm via the config double-buffer.
    cfg::RuntimeConfig* sc = cfg::Config::editScratch();
    sc->pulses_per_mm = ppm;
    cfg::Config::publish();
    pattern::onConfigApplied();

    evt::Event e{}; e.kind = evt::Kind::CalibResult; e.f1 = ppm;
    BaseType_t hp = pdFALSE;
    evt::postFromISR(e, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// ---------------- PatternTask: poll events ----------------
static void patternTask(void*) {
    uint32_t lastPulse = 0;
    int64_t  lastUs    = esp_timer_get_time();
    int64_t  lastLoopUs = lastUs;
    bool     wasActive = false;

    for (;;) {
        uint32_t now = encoder::pulseCount();

        // --- speed estimate (mm/s) ---
        int64_t nowUs = esp_timer_get_time();
        bool active = cfg::g_sys.active.load(std::memory_order_acquire);
        if (active && wasActive) updateMax(s_maxLoopGapUs, (uint32_t)(nowUs - lastLoopUs));
        lastLoopUs = nowUs;
        wasActive = active;
        int64_t dtUs  = nowUs - lastUs;
        if (dtUs >= 10000) {
            uint32_t dp = now - lastPulse;
            float mm   = pulsesToMm(dp);
            float sec  = (float)dtUs * 1e-6f;
            s_lastSpeedMmS.store(mm / sec, std::memory_order_release);
            lastPulse = now;
            lastUs    = nowUs;
        }

        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // --- speed safety: disable firing if too slow ---
        bool tooSlow = s_lastSpeedMmS.load(std::memory_order_acquire) <
                       cfg::Config::active()->min_speed_mm_s;

        for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
            // Pull oldest instance for this gun (read-only peek; we may pop
            // after firing under the spinlock).
            SheetGunInstance peek;
            bool have = false;
            portENTER_CRITICAL(&s_mux);
            if (s_q[g].size > 0) { peek = s_q[g].ring[s_q[g].head]; have = true; }
            portEXIT_CRITICAL(&s_mux);
            if (!have) continue;

            // Drain all events for this instance that have come due, in order.
            while (have) {
                uint32_t evPulse;
                uint8_t  action;
                if (!nextEventPulse(g, peek, evPulse, action)) {
                    // Instance has no more events -> pop and check next.
                    portENTER_CRITICAL(&s_mux);
                    s_q[g].head = (s_q[g].head + 1) % SHEET_QUEUE_DEPTH;
                    s_q[g].size--;
                    bool more = s_q[g].size > 0;
                    if (more) peek = s_q[g].ring[s_q[g].head];
                    portEXIT_CRITICAL(&s_mux);
                    have = more;
                    continue;
                }
                int32_t latePulses = (int32_t)(now - evPulse);
                if (latePulses < 0) break;   // not due yet
                updateMax(s_maxEventLatePulses, (uint32_t)latePulses);
                s_patternEvents.fetch_add(1, std::memory_order_relaxed);

                if (!tooSlow) {
                    if (action == 0) {                     // dot
                        seq::fire(g, 0);                   // on-time = pattern[g].on_timeout_ms
                    } else if (action == 1) {              // open line
                        seq::fire(g, 5000);                // long; closed by action 2
                    } else if (action == 2) {              // close line
                        seq::abort(g);
                    }
                }
                advanceInstance(g, peek, action);
                // Write back the advanced instance.
                portENTER_CRITICAL(&s_mux);
                if (s_q[g].size > 0) s_q[g].ring[s_q[g].head] = peek;
                portEXIT_CRITICAL(&s_mux);
            }
        }

        vTaskDelay(1);     // ~1 ms tick on Core 1.  Plenty for 330 Hz droplets.
    }
}

// ---------------- public API ----------------
void onConfigApplied() {
    const cfg::RuntimeConfig* c = cfg::Config::active();
    s_pulsesPerMm.store(c->pulses_per_mm,       std::memory_order_release);
    s_offsetMm   .store(c->photocell_offset_mm, std::memory_order_release);
}

void onCalibArm(float paperLengthMm) {
    s_calibPaperLen.store(paperLengthMm, std::memory_order_release);
    s_calib.store(CalibState::ArmedLeading, std::memory_order_release);
}

void abortAll() {
    portENTER_CRITICAL(&s_mux);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) { s_q[g].head = 0; s_q[g].size = 0; }
    portEXIT_CRITICAL(&s_mux);
    seq::abortAll();
}

float currentPosMm()   { return pulsesToMm(encoder::pulseCount()); }
float currentSpeedMmS(){ return s_lastSpeedMmS.load(std::memory_order_acquire); }
Metrics metrics() {
    return {
        s_maxLoopGapUs.load(std::memory_order_relaxed),
        s_maxEventLatePulses.load(std::memory_order_relaxed),
        s_patternEvents.load(std::memory_order_relaxed),
        s_sheetQueueOverflows.load(std::memory_order_relaxed),
    };
}

void init() {
    onConfigApplied();
    xTaskCreatePinnedToCore(patternTask, "pattern", 8192, nullptr, 7, nullptr, 1);
}

} // namespace pattern
