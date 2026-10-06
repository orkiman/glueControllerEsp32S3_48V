#include "Events.h"
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <string.h>

namespace evt {

static constexpr UBaseType_t QUEUE_LEN = 32;
static QueueHandle_t s_queue = nullptr;
static EventCallback s_cb = nullptr;
static void* s_cbUser = nullptr;

// Staging for snapshot events that are too big for a queue element.  The
// emitter may still be writing a previous line to Serial when the next post
// arrives, so each kind (and each gun) has its own slot, guarded by a mutex.
// A slot always holds the latest snapshot; a duplicate queued event simply
// re-sends that latest state.
static SemaphoreHandle_t  s_stageMtx = nullptr;

static prog::ProgramMeta  s_plList[prog::MAX_PROGRAMS];
static size_t             s_plCount = 0;
static uint8_t            s_plActive = 0;

static cfg::RuntimeConfig s_evConfig;
static cfg::GunPattern    s_evPattern[pins::NUM_GUNS];

class StageLock {
public:
    StageLock()  { if (s_stageMtx) xSemaphoreTake(s_stageMtx, portMAX_DELAY); }
    ~StageLock() { if (s_stageMtx) xSemaphoreGive(s_stageMtx); }
};

static inline void invokeCb(const Event& e) {
    if (s_cb) s_cb(e, s_cbUser);
}

static void emitterTask(void*) {
    Event e;
    JsonDocument doc;
    // Static, large buffer so a full 64-element pattern fits without truncation.
    static char line[8192];
    // Private copies of staged snapshots, taken under the stage lock.
    static prog::ProgramMeta  plList[prog::MAX_PROGRAMS];
    static cfg::RuntimeConfig evConfig;
    static cfg::GunPattern    evPattern;

    for (;;) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdTRUE) continue;

        doc.clear();
        switch (e.kind) {
            case Kind::Ready:
                doc["event"] = "ready";
                break;
            case Kind::Ack:
                doc["event"] = "ack";
                doc["cmd"]   = e.cmd;
                break;
            case Kind::Error:
                doc["event"]  = "error";
                doc["cmd"]    = e.cmd;
                doc["reason"] = e.reason;
                break;
            case Kind::CalibResult:
                doc["event"]         = "calib_result";
                doc["pulses_per_mm"] = e.f1;
                break;
            case Kind::Status:
                doc["event"]                 = "status";
                doc["pos_mm"]                = e.f1;
                doc["speed_mm_s"]            = e.f2;
                doc["active"]                = e.b1 != 0;
                doc["max_loop_gap_us"]       = e.u1;
                doc["max_event_late_pulses"] = e.u2;
                doc["pattern_events"]        = e.u3;
                doc["sheet_queue_overflows"] = e.u4;
                doc["sheet_count"]           = e.u5;
                break;
            case Kind::WatchdogTimeout:
                doc["event"] = "watchdog_timeout";
                break;
            case Kind::Debug:
                doc["event"] = "debug";
                doc["tag"]   = e.cmd;      // "peak" / "nopeak"
                doc["gun"]   = e.b1;       // 1-based gun
                doc["us"]    = e.f1;       // microseconds since fire()
                break;
            case Kind::ProgramList: {
                size_t  count;
                uint8_t activeId;
                {
                    StageLock lock;
                    count    = s_plCount;
                    activeId = s_plActive;
                    for (size_t i = 0; i < count; ++i) plList[i] = s_plList[i];
                }
                doc["event"]     = "programs_list";
                doc["active_id"] = activeId;
                JsonArray arr = doc["programs"].to<JsonArray>();
                for (size_t i = 0; i < count; ++i) {
                    JsonObject o = arr.add<JsonObject>();
                    o["id"]   = plList[i].id;
                    o["name"] = plList[i].name;
                }
                break;
            }
            case Kind::Config:
                {
                    StageLock lock;
                    evConfig = s_evConfig;
                }
                doc["event"]                = "config";
                doc["pulses_per_mm"]        = evConfig.pulses_per_mm;
                doc["min_speed_mm_s"]       = evConfig.min_speed_mm_s;
                doc["photocell_offset_mm"]  = evConfig.photocell_offset_mm;
                doc["debounce_ms"]          = evConfig.debounce_ms;
                doc["pick_current_a"]       = evConfig.pick_current_a;
                doc["hold_current_a"]       = evConfig.hold_current_a;
                doc["encoder_source"]       = evConfig.encoder_source;
                break;
            case Kind::Pattern: {
                uint8_t gun = e.b1;          // 1-based
                if (gun < 1 || gun > pins::NUM_GUNS) continue;
                {
                    StageLock lock;
                    evPattern = s_evPattern[gun - 1];
                }
                doc["event"]          = "pattern";
                doc["gun"]            = gun;
                doc["type"]           = (evPattern.type == cfg::PatternType::Lines) ? "lines" :
                                        (evPattern.type == cfg::PatternType::Dots)  ? "dots" : "none";
                doc["on_timeout_ms"]  = evPattern.on_timeout_ms;
                JsonArray arr = doc["elements"].to<JsonArray>();
                for (uint8_t i = 0; i < evPattern.count; ++i) {
                    JsonObject o = arr.add<JsonObject>();
                    o["start"] = evPattern.elems[i].start_mm;
                    o["end"]   = evPattern.elems[i].end_mm;
                    if (evPattern.type == cfg::PatternType::Dots) {
                        o["spacing"] = evPattern.elems[i].spacing_mm;
                    }
                }
                break;
            }
        }
        size_t n = serializeJson(doc, line, sizeof(line) - 2);
        line[n++] = '\n';
        line[n]   = '\0';
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
}

void init() {
    if (s_queue) return;
    s_stageMtx = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(Event));
    xTaskCreatePinnedToCore(emitterTask, "evt_emit", 4096, nullptr, 5, nullptr, 0);
}

bool post(const Event& e) {
    if (!s_queue) return false;
    bool ok = xQueueSend(s_queue, &e, 0) == pdTRUE;
    if (ok) invokeCb(e);
    return ok;
}

bool postFromISR(const Event& e, BaseType_t* hpWoken) {
    if (!s_queue) return false;
    bool ok = xQueueSendFromISR(s_queue, &e, hpWoken) == pdTRUE;
    if (ok) invokeCb(e);
    return ok;
}

void setCallback(EventCallback cb, void* user) {
    s_cb = cb;
    s_cbUser = user;
}

static inline void setStr(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void postReady() {
    Event e{}; e.kind = Kind::Ready; post(e);
}
void postAck(const char* cmd) {
    Event e{}; e.kind = Kind::Ack; setStr(e.cmd, sizeof(e.cmd), cmd); post(e);
}
void postError(const char* cmd, const char* reason) {
    Event e{}; e.kind = Kind::Error;
    setStr(e.cmd,    sizeof(e.cmd),    cmd);
    setStr(e.reason, sizeof(e.reason), reason);
    post(e);
}
void postCalibResult(float pulses_per_mm) {
    Event e{}; e.kind = Kind::CalibResult; e.f1 = pulses_per_mm; post(e);
}
void postProgramsList(const prog::ProgramMeta* list, size_t count, uint8_t activeId) {
    size_t n = (count > prog::MAX_PROGRAMS) ? prog::MAX_PROGRAMS : count;
    {
        StageLock lock;
        for (size_t i = 0; i < n; ++i) s_plList[i] = list[i];
        s_plCount = n;
        s_plActive = activeId;
    }
    Event e{}; e.kind = Kind::ProgramList; post(e);
}
void postConfig(const cfg::RuntimeConfig* config) {
    {
        StageLock lock;
        s_evConfig = *config;
    }
    Event e{}; e.kind = Kind::Config; post(e);
}
void postPattern(uint8_t gun_1based, const cfg::GunPattern* pattern) {
    if (gun_1based < 1 || gun_1based > pins::NUM_GUNS) return;
    {
        StageLock lock;
        s_evPattern[gun_1based - 1] = *pattern;
    }
    Event e{}; e.kind = Kind::Pattern; e.b1 = gun_1based; post(e);
}
void postWatchdogTimeout() {
    Event e{}; e.kind = Kind::WatchdogTimeout; post(e);
}
void postStatus(float pos_mm, float speed_mm_s, bool active,
                uint32_t maxLoopGapUs, uint32_t maxEventLatePulses,
                uint32_t patternEvents, uint32_t sheetQueueOverflows,
                uint32_t sheetCount) {
    Event e{}; e.kind = Kind::Status;
    e.f1 = pos_mm; e.f2 = speed_mm_s; e.b1 = active ? 1 : 0;
    e.u1 = maxLoopGapUs;
    e.u2 = maxEventLatePulses;
    e.u3 = patternEvents;
    e.u4 = sheetQueueOverflows;
    e.u5 = sheetCount;
    post(e);
}

} // namespace evt
