#pragma once
#include <Arduino.h>
#include <atomic>
#include "hw/Pins.h"

// =============================================================================
// Config & Pattern storage with lock-free, double-buffered publication.
//
// Hot path (Core 1) reads via Config::active() returning a const* snapshot.
// Cold path (Core 0 / JSON task) builds the next snapshot in the inactive
// buffer then publishes with Config::publish().
//
// Patterns are stored inline inside RuntimeConfig so a single pointer swap
// publishes config + patterns atomically.
// =============================================================================

namespace cfg {

constexpr size_t MAX_PATTERN_ELEMENTS_PER_GUN = 64;

struct PatternElement {
    float    start_mm;
    float    end_mm;
    float    spacing_mm;   // 0 -> line, >0 -> dots
};

enum class PatternType : uint8_t { None = 0, Lines = 1, Dots = 2 };

struct GunPattern {
    PatternType    type         = PatternType::None;
    uint8_t        count        = 0;
    // Total on-time budget for one fire cycle (ms), measured from the moment
    // seq::fire() is called -- i.e. from the start of the Peak phase, NOT
    // from the LM339 peak-current trip. When this elapses the sequencer is
    // forced into Phase 3 (Active Fast Decay) regardless of which phase it
    // is currently in. This also serves as the fail-safe if peak current
    // is never reached (open coil, broken sense path, etc.).
    //   * Dots mode  -> droplet on-time (the user-facing "drop size").
    //   * Lines mode -> ignored for normal pattern execution (lines end on
    //                   encoder position via seq::abort). Still acts as the
    //                   global 5 s safety ceiling enforced inside fire().
    float          on_timeout_ms = 1.2f;
    PatternElement elems[MAX_PATTERN_ELEMENTS_PER_GUN];
};

struct RuntimeConfig {
    // ---- set_config payload (global) ----
    float    pulses_per_mm        = 1.0f;
    float    min_speed_mm_s       = 100.0f;
    float    photocell_offset_mm  = 250.0f;
    uint32_t debounce_ms          = 20;
    float    pick_current_a       = 1.0f;
    float    hold_current_a       = 0.4f;

    // ---- per-gun pattern (includes per-gun on_timeout_ms / droplet size) ----
    GunPattern pattern[pins::NUM_GUNS];
};

class Config {
public:
    static void init();                          // populates defaults in both buffers
    static const RuntimeConfig* active();        // hot-path read (IRAM-safe)
    static RuntimeConfig*       editScratch();   // cold-path write target
    static void publish();                       // atomic swap of active pointer

private:
    static RuntimeConfig             buf_[2];
    static std::atomic<uint8_t>      activeIdx_;
};

// ---- Runtime/system state (separate from config) ----
struct SystemState {
    std::atomic<bool>     active{false};        // mirrors HMI set_active command
    std::atomic<bool>     fault{false};         // nFAULT latched
    std::atomic<uint32_t> lastCmdMs{0};         // watchdog refresh timestamp
};

extern SystemState g_sys;

} // namespace cfg
