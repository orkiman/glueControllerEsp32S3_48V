#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "config/Config.h"
#include "storage/ProgramStore.h"

// =============================================================================
// Cross-core event pipe: Core 1 (ISRs / RT tasks) -> Core 0 (UART emitter).
//
// All payloads are POD and small (<= 32 bytes) so xQueueSendFromISR is cheap.
// The emitter task drains the queue and formats NDJSON on Core 0 only.
// No module other than EmitterTask is allowed to touch Serial.
// =============================================================================

namespace evt {

enum class Kind : uint8_t {
    Ready = 0,           // {"event":"ready"}
    Ack,                 // {"event":"ack","cmd":"..."}
    Error,               // {"event":"error","cmd":"..","reason":".."}
    CalibResult,         // {"event":"calib_result","pulses_per_mm":X}
    Status,              // {"event":"status",...}
    WatchdogTimeout,     // {"event":"watchdog_timeout"}
    Debug,               // {"event":"debug","tag":"..","gun":N,"us":X}
    ProgramList,         // {"event":"programs_list","programs":[...],"active_id":N}
    Config,              // {"event":"config",...}
    Pattern,             // {"event":"pattern",...}
};

// Short fixed-width string fields keep the queue element trivially copyable.
struct Event {
    Kind    kind;
    char    cmd[20];      // for Ack / Error
    char    reason[24];   // for Error
    float   f1;           // CalibResult: pulses_per_mm ; Status: pos_mm
    float   f2;           // Status: speed_mm_s
    uint8_t b1;           // Status: active flag
    uint32_t u1;
    uint32_t u2;
    uint32_t u3;
    uint32_t u4;
};

void init();                                  // creates queue + spawns emitter
bool post(const Event& e);                    // task context
bool postFromISR(const Event& e, BaseType_t* hpWoken);

// Optional second subscriber (e.g. HTTP server). Called from task and ISR ctx.
using EventCallback = void(*)(const Event&, void*);
void setCallback(EventCallback cb, void* user = nullptr);

// --- Convenience helpers (task context only) ---
void postReady();
void postAck(const char* cmd);
void postError(const char* cmd, const char* reason);
void postCalibResult(float pulses_per_mm);
void postProgramsList(const prog::ProgramMeta* list, size_t count, uint8_t activeId);
void postConfig(const cfg::RuntimeConfig* config);
void postPattern(uint8_t gun_1based, const cfg::GunPattern* pattern);
void postWatchdogTimeout();
void postStatus(float pos_mm, float speed_mm_s, bool active,
                uint32_t maxLoopGapUs, uint32_t maxEventLatePulses,
                uint32_t patternEvents, uint32_t sheetQueueOverflows);

} // namespace evt
