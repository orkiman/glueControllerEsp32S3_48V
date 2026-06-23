#include "UartJson.h"
#include "Events.h"
#include "config/Config.h"
#include "rt/Control.h"
#include "hw/Pins.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace uartjson {

static constexpr size_t LINE_CAP = 1024;

// -------- helpers ----------------------------------------------------------

static inline void feedWatchdog() {
    cfg::g_sys.lastCmdMs.store(millis(), std::memory_order_release);
}

static bool parsePatternElements(JsonArrayConst arr, cfg::PatternType type,
                                 cfg::GunPattern& out, const char*& reason) {
    if (arr.size() > cfg::MAX_PATTERN_ELEMENTS_PER_GUN) {
        reason = "too_many_elements"; return false;
    }
    out.type  = type;
    out.count = 0;
    for (JsonObjectConst el : arr) {
        if (!el["start"].is<float>() || !el["end"].is<float>()) {
            reason = "missing_start_end"; return false;
        }
        cfg::PatternElement& pe = out.elems[out.count];
        pe.start_mm   = el["start"].as<float>();
        pe.end_mm     = el["end"].as<float>();
        pe.spacing_mm = (type == cfg::PatternType::Dots)
                            ? el["spacing"].as<float>()
                            : 0.0f;
        if (pe.end_mm < pe.start_mm) { reason = "end_before_start"; return false; }
        if (type == cfg::PatternType::Dots && pe.spacing_mm <= 0.0f) {
            reason = "bad_spacing"; return false;
        }
        out.count++;
    }
    return true;
}

// -------- per-command handlers --------------------------------------------

static void handleSetActive(JsonDocument& doc) {
    if (!doc["active"].is<bool>()) {
        evt::postError("set_active", "missing_active"); return;
    }
    bool a = doc["active"].as<bool>();
    if (a && cfg::g_sys.fault.load()) {
        // Operator re-arm clears the latched fault flag.
        cfg::g_sys.fault.store(false);
    }
    rt::onSetActive(a);
    cfg::g_sys.active.store(a);
    evt::postAck("set_active");
}

static void handleSetConfig(JsonDocument& doc) {
    cfg::RuntimeConfig* s = cfg::Config::editScratch();
    if (doc["pulses_per_mm"].is<float>())       s->pulses_per_mm       = doc["pulses_per_mm"];
    if (doc["min_speed_mm_s"].is<float>())      s->min_speed_mm_s      = doc["min_speed_mm_s"];
    if (doc["photocell_offset_mm"].is<float>()) s->photocell_offset_mm = doc["photocell_offset_mm"];
    if (doc["debounce_ms"].is<uint32_t>())      s->debounce_ms         = doc["debounce_ms"];
    if (doc["pick_current_a"].is<float>())      s->pick_current_a      = doc["pick_current_a"];
    if (doc["hold_current_a"].is<float>())      s->hold_current_a      = doc["hold_current_a"];

    // Basic sanity
    if (s->pulses_per_mm   <= 0.0f) { evt::postError("set_config","bad_pulses_per_mm");  return; }
    if (s->pick_current_a  <= 0.0f) { evt::postError("set_config","bad_pick_current");   return; }
    if (s->hold_current_a  <= 0.0f) { evt::postError("set_config","bad_hold_current");   return; }
    if (s->hold_current_a  >= s->pick_current_a) {
        evt::postError("set_config","hold_ge_pick");                                    return; }

    cfg::Config::publish();
    rt::onConfigApplied();
    evt::postAck("set_config");
}

static void handleSetPattern(JsonDocument& doc) {
    if (!doc["gun"].is<uint8_t>()) { evt::postError("set_pattern","missing_gun"); return; }
    uint8_t gunOneBased = doc["gun"].as<uint8_t>();
    if (gunOneBased < 1 || gunOneBased > pins::NUM_GUNS) {
        evt::postError("set_pattern","invalid_gun"); return;
    }
    const char* typeStr = doc["type"] | "";
    cfg::PatternType type;
    if      (!strcmp(typeStr, "lines")) type = cfg::PatternType::Lines;
    else if (!strcmp(typeStr, "dots"))  type = cfg::PatternType::Dots;
    else if (!strcmp(typeStr, "none"))  type = cfg::PatternType::None;
    else { evt::postError("set_pattern","bad_type"); return; }

    cfg::RuntimeConfig* s = cfg::Config::editScratch();
    cfg::GunPattern&    gp = s->pattern[gunOneBased - 1];

    if (type == cfg::PatternType::None) {
        gp.type  = cfg::PatternType::None;
        gp.count = 0;
    } else {
        JsonArrayConst arr = doc["elements"].as<JsonArrayConst>();
        if (arr.isNull()) { evt::postError("set_pattern","missing_elements"); return; }

        const char* reason = nullptr;
        if (!parsePatternElements(arr, type, gp, reason)) {
            evt::postError("set_pattern", reason ? reason : "parse_error"); return;
        }
    }
    // Per-gun on-timeout (covers Peak+Hold; doubles as droplet size in Dots
    // mode and as the long safety ceiling in Lines mode).  Optional --
    // absent fields preserve the previous value via editScratch() copy.
    if (doc["on_timeout_ms"].is<float>()) {
        float ot = doc["on_timeout_ms"].as<float>();
        if (ot <= 0.0f) { evt::postError("set_pattern","bad_on_timeout"); return; }
        gp.on_timeout_ms = ot;
    }
    cfg::Config::publish();
    evt::postAck("set_pattern");
}

static void handleCalibArm(JsonDocument& doc) {
    if (!doc["paper_length_mm"].is<float>()) {
        evt::postError("calib_arm","missing_paper_length"); return;
    }
    float L = doc["paper_length_mm"].as<float>();
    if (L <= 0.0f) { evt::postError("calib_arm","bad_paper_length"); return; }
    rt::onCalibArm(L);
    evt::postAck("calib_arm");
}

static void handleTestOpen(JsonDocument& doc) {
    if (!doc["gun"].is<uint8_t>()) { evt::postError("test_open","missing_gun"); return; }
    uint8_t  gun     = doc["gun"].as<uint8_t>();         // 0 = all, 1..4 = single
    uint32_t timeout = doc["timeout_ms"] | 1000u;
    if (gun > pins::NUM_GUNS)        { evt::postError("test_open","invalid_gun");     return; }
    if (timeout == 0 || timeout > 5000u) {
        evt::postError("test_open","bad_timeout"); return;
    }
    if (cfg::g_sys.fault.load())     { evt::postError("test_open","hardware_fault");  return; }
    rt::onTestOpen(gun, timeout);
    evt::postAck("test_open");
}

static void handleTestClose(JsonDocument& doc) {
    uint8_t gun = doc["gun"] | 0;
    if (gun > pins::NUM_GUNS) { evt::postError("test_close","invalid_gun"); return; }
    rt::onTestClose(gun);
    evt::postAck("test_close");
}

static void handlePing(JsonDocument&) {
    // Watchdog feed already happened in dispatch().
    evt::postAck("ping");
}

static void handleSwTrigger(JsonDocument&) {
    if (cfg::g_sys.fault.load()) { evt::postError("sw_trigger","hardware_fault"); return; }
    rt::onSwTrigger();
    evt::postAck("sw_trigger");
}

// -------- dispatch ---------------------------------------------------------

static void dispatch(const char* line, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line, len);
    if (err) { evt::postError("", "bad_json"); return; }

    const char* cmd = doc["cmd"] | "";
    feedWatchdog();

    if      (!strcmp(cmd, "set_active"))  handleSetActive(doc);
    else if (!strcmp(cmd, "set_config"))  handleSetConfig(doc);
    else if (!strcmp(cmd, "set_pattern")) handleSetPattern(doc);
    else if (!strcmp(cmd, "calib_arm"))   handleCalibArm(doc);
    else if (!strcmp(cmd, "test_open"))   handleTestOpen(doc);
    else if (!strcmp(cmd, "test_close"))  handleTestClose(doc);
    else if (!strcmp(cmd, "ping"))        handlePing(doc);
    else if (!strcmp(cmd, "sw_trigger"))  handleSwTrigger(doc);
    else                                  evt::postError(cmd, "unknown_cmd");
}

// -------- RX task ----------------------------------------------------------

static void rxTask(void*) {
    static char   line[LINE_CAP];
    static size_t len = 0;
    bool overflow = false;

    for (;;) {
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0) break;

            if (c == '\r') continue;
            if (c == '\n') {
                if (overflow) {
                    evt::postError("", "line_too_long");
                } else if (len > 0) {
                    line[len] = '\0';
                    dispatch(line, len);
                }
                len = 0;
                overflow = false;
                continue;
            }
            if (len < LINE_CAP - 1) line[len++] = (char)c;
            else                    overflow = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void init() {
    Serial.begin(115200, SERIAL_8N1, pins::UART_RX, pins::UART_TX);
    Serial.setRxBufferSize(2048);
    xTaskCreatePinnedToCore(rxTask, "uart_rx", 8192, nullptr, 4, nullptr, 0);
}

} // namespace uartjson
