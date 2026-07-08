#include "Events.h"
#include <ArduinoJson.h>
#include <string.h>

namespace evt {

static constexpr UBaseType_t QUEUE_LEN = 32;
static QueueHandle_t s_queue = nullptr;

static void emitterTask(void*) {
    Event e;
    JsonDocument doc;
    char line[192];

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
                doc["event"]      = "status";
                doc["pos_mm"]     = e.f1;
                doc["speed_mm_s"] = e.f2;
                doc["active"]     = e.b1 != 0;
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
        }
        size_t n = serializeJson(doc, line, sizeof(line) - 2);
        line[n++] = '\n';
        line[n]   = '\0';
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
}

void init() {
    if (s_queue) return;
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(Event));
    xTaskCreatePinnedToCore(emitterTask, "evt_emit", 4096, nullptr, 5, nullptr, 0);
}

bool post(const Event& e) {
    if (!s_queue) return false;
    return xQueueSend(s_queue, &e, 0) == pdTRUE;
}

bool postFromISR(const Event& e, BaseType_t* hpWoken) {
    if (!s_queue) return false;
    return xQueueSendFromISR(s_queue, &e, hpWoken) == pdTRUE;
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
void postWatchdogTimeout() {
    Event e{}; e.kind = Kind::WatchdogTimeout; post(e);
}
void postStatus(float pos_mm, float speed_mm_s, bool active) {
    Event e{}; e.kind = Kind::Status;
    e.f1 = pos_mm; e.f2 = speed_mm_s; e.b1 = active ? 1 : 0;
    post(e);
}

} // namespace evt
