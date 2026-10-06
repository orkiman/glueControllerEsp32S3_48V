#include "PatternScheduler.h"
#include "GunSequencer.h"
#include "hw/Encoder.h"
#include "hw/Pins.h"
#include "config/Config.h"
#include "comms/Events.h"
#include "comms/LiveSync.h"

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

// Lines mode: gun is inside a line region but held closed because speed is
// below min_speed_mm_s.  Re-opened when speed recovers.  patternTask-only.
static bool                  s_linePaused[pins::NUM_GUNS] = {};

// Resume threshold = min speed * this, so a speed reading hovering right at
// the limit doesn't chatter the solenoid on/off.
static constexpr float       RESUME_HYSTERESIS = 1.05f;

// ---------------- cached config scalars ----------------
static std::atomic<float>    s_pulsesPerMm{12.34f};
static std::atomic<float>    s_offsetMm{250.0f};

// ---------------- calibration state ----------------
enum class CalibState : uint8_t { Idle = 0, ArmedLeading = 1, ArmedTrailing = 2 };
static std::atomic<CalibState> s_calib{CalibState::Idle};
static std::atomic<float>      s_calibPaperLen{0.0f};
static std::atomic<uint32_t>   s_calibLeadPulse{0};

// ---------------- speed estimation ----------------
// Two independent measurements, both from polling the PCNT count in
// patternTask (no per-pulse interrupt):
//  - Display speed: pulses over a 300 ms sliding window, sampled every 10 ms.
//    Smooth, shown in GUI / web.  Too laggy for the safety gate.
//  - Safety gate (lines min speed): every count change seen by the ~1 ms poll
//    is logged with its poll time t and the poll gap d -- the pulse happened
//    in (t - d, t].  The gate works on guaranteed bounds from that log, so it
//    never trips on quantisation and reacts within one pulse period.
static constexpr uint8_t  SPEED_SAMPLES   = 30;        // x 10 ms = 300 ms
static constexpr int64_t  SPEED_SAMPLE_US = 10000;
static constexpr uint8_t  EDGE_LOG        = 64;
static constexpr int64_t  GATE_WINDOW_US  = 30000;

struct SpeedSample { uint32_t count; int64_t tUs; };
struct EdgeRec     { uint32_t count; int64_t tUs; int64_t dUs; };

// patternTask-only state.
static SpeedSample s_samples[SPEED_SAMPLES + 1];
static uint8_t     s_sampleHead = 0;         // newest
static uint8_t     s_sampleCount = 0;
static EdgeRec     s_edges[EDGE_LOG];
static uint8_t     s_edgeHead = 0;           // newest
static uint8_t     s_edgeCount = 0;

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

// ---------------- speed monitoring (patternTask context) ----------------
// Push a 300 ms-window sample and return the window's average speed (mm/s).
static float sampleDisplaySpeed(uint32_t count, int64_t nowUs) {
    s_sampleHead = (s_sampleHead + 1) % (SPEED_SAMPLES + 1);
    s_samples[s_sampleHead] = { count, nowUs };
    if (s_sampleCount < SPEED_SAMPLES + 1) s_sampleCount++;
    if (s_sampleCount < 2) return 0.0f;

    const SpeedSample& oldest =
        s_samples[(s_sampleHead + SPEED_SAMPLES + 2 - s_sampleCount) % (SPEED_SAMPLES + 1)];
    float sec = (float)(nowUs - oldest.tUs) * 1e-6f;
    return (sec > 0.0f) ? pulsesToMm(count - oldest.count) / sec : 0.0f;
}

static void logEdge(uint32_t count, int64_t tUs, int64_t dUs) {
    s_edgeHead = (s_edgeHead + 1) % EDGE_LOG;
    s_edges[s_edgeHead] = { count, tUs, dUs };
    if (s_edgeCount < EDGE_LOG) s_edgeCount++;
}

// Lines min-speed verdict.  Neither flag set = ambiguous (speed close to the
// limit); the caller keeps its current state, which is the hysteresis.
struct SpeedGate { bool tooSlow; bool canResume; };

static SpeedGate speedGate(int64_t nowUs, float minSpeed) {
    if (minSpeed <= 0.0f) return { false, true };
    float ppm = s_pulsesPerMm.load(std::memory_order_acquire);
    if (ppm <= 0.0f || s_edgeCount == 0) return { true, false };

    const EdgeRec& newest = s_edges[s_edgeHead];

    // 1) No pulse for longer than one pulse period at minSpeed: the speed is
    //    below minSpeed right now (poll time <= true time since the pulse).
    float gapAtMinUs = 1e6f / (ppm * minSpeed);
    if ((float)(nowUs - newest.tUs) > gapAtMinUs) return { true, false };

    // 2) Reference edge: oldest one inside the gate window, or the one just
    //    before `newest` if the window holds only `newest`.
    const EdgeRec* old = nullptr;
    for (uint8_t i = 1; i < s_edgeCount; ++i) {
        const EdgeRec& e = s_edges[(s_edgeHead + EDGE_LOG - i) % EDGE_LOG];
        if (old && nowUs - e.tUs > GATE_WINDOW_US) break;
        old = &e;
    }
    if (!old) return { false, false };

    float pulses = (float)(newest.count - old->count);
    // Upper bound on speed between the two edges -> certainly too slow?
    float minDurUs = (float)(newest.tUs - old->tUs - newest.dUs);
    bool  slow = minDurUs > 0.0f && pulses * 1e6f / (ppm * minDurUs) < minSpeed;
    // Lower bound on speed from the reference edge until now -> certainly fast?
    float maxDurUs = (float)(nowUs - old->tUs + old->dUs);
    bool  fast = pulses * 1e6f / (ppm * maxDurUs) >= minSpeed * RESUME_HYSTERESIS;
    return { slow, !slow && fast };
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

    // Applying pulses_per_mm to the config double-buffer happens in task
    // context (livesync::service) under the shared edit lock, so an ISR never
    // races a UART / web edit of the scratch buffer.
    livesync::postCalibration(ppm);

    evt::Event e{}; e.kind = evt::Kind::CalibResult; e.f1 = ppm;
    BaseType_t hp = pdFALSE;
    evt::postFromISR(e, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// ---------------- PatternTask: poll events ----------------
static void patternTask(void*) {
    uint32_t lastSeen   = encoder::pulseCount();
    int64_t  lastSampleUs = esp_timer_get_time();
    int64_t  lastLoopUs = lastSampleUs;
    bool     wasActive = false;

    for (;;) {
        uint32_t now = encoder::pulseCount();

        // --- speed monitoring ---
        int64_t nowUs = esp_timer_get_time();
        int64_t pollGapUs = nowUs - lastLoopUs;
        bool active = cfg::g_sys.active.load(std::memory_order_acquire);
        if (active && wasActive) updateMax(s_maxLoopGapUs, (uint32_t)pollGapUs);
        lastLoopUs = nowUs;
        wasActive = active;
        if (now != lastSeen) {
            logEdge(now, nowUs, pollGapUs);
            lastSeen = now;
        }
        if (nowUs - lastSampleUs >= SPEED_SAMPLE_US) {
            s_lastSpeedMmS.store(sampleDisplaySpeed(now, nowUs), std::memory_order_release);
            lastSampleUs = nowUs;
        }

        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // --- speed safety: lines only.  Below min speed an open line is
        //     closed immediately and re-opened if speed recovers while still
        //     inside the line region.  Dots fire at any speed. ---
        SpeedGate gate = speedGate(nowUs, cfg::Config::active()->min_speed_mm_s);
        bool tooSlow   = gate.tooSlow;
        bool canResume = gate.canResume;

        for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
            // Pull oldest instance for this gun (read-only peek; we may pop
            // after firing under the spinlock).
            SheetGunInstance peek;
            bool have = false;
            portENTER_CRITICAL(&s_mux);
            if (s_q[g].size > 0) { peek = s_q[g].ring[s_q[g].head]; have = true; }
            portEXIT_CRITICAL(&s_mux);
            if (!have) { s_linePaused[g] = false; continue; }

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

                if (action == 0) {                         // dot
                    seq::fire(g, 0);                       // on-time = pattern[g].on_timeout_ms
                } else if (action == 1) {                  // open line
                    if (tooSlow) s_linePaused[g] = true;   // opened later if speed recovers
                    else         seq::fire(g, 5000);       // long; closed by action 2
                } else if (action == 2) {                  // close line (always; no-op if idle)
                    s_linePaused[g] = false;
                    seq::abort(g);
                }
                advanceInstance(g, peek, action);
                // Write back the advanced instance.
                portENTER_CRITICAL(&s_mux);
                if (s_q[g].size > 0) s_q[g].ring[s_q[g].head] = peek;
                portEXIT_CRITICAL(&s_mux);
            }

            // Mid-line speed gating: `peek.lineOpen` means the head sheet is
            // between start_mm and end_mm of a line on this gun.
            if (have && peek.lineOpen) {
                if (tooSlow) {
                    if (!s_linePaused[g]) { seq::abort(g); s_linePaused[g] = true; }
                } else if (s_linePaused[g] && canResume) {
                    // fire() fails while the coil is still decaying from the
                    // abort; stay paused and retry next tick.
                    if (seq::fire(g, 5000)) s_linePaused[g] = false;
                }
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
