#include "CommandDispatcher.h"
#include "config/Config.h"
#include "rt/Control.h"
#include "storage/ProgramStore.h"
#include "hw/Pins.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace cmd {

static inline void setStr(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static inline Result makeResult(bool ok, const char* cmd, const char* reason = "") {
    Result r{};
    r.ok = ok;
    setStr(r.cmd, sizeof(r.cmd), cmd);
    setStr(r.reason, sizeof(r.reason), reason);
    return r;
}

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

static Result handleSetActive(JsonDocument& doc) {
    if (!doc["active"].is<bool>()) {
        return makeResult(false, "set_active", "missing_active");
    }
    bool a = doc["active"].as<bool>();
    if (a && cfg::g_sys.fault.load()) {
        cfg::g_sys.fault.store(false);
    }
    rt::onSetActive(a);
    cfg::g_sys.active.store(a);
    return makeResult(true, "set_active");
}

static Result handleSetConfig(JsonDocument& doc) {
    cfg::RuntimeConfig* s = cfg::Config::editScratch();
    if (doc["pulses_per_mm"].is<float>())       s->pulses_per_mm       = doc["pulses_per_mm"];
    if (doc["min_speed_mm_s"].is<float>())      s->min_speed_mm_s      = doc["min_speed_mm_s"];
    if (doc["photocell_offset_mm"].is<float>()) s->photocell_offset_mm = doc["photocell_offset_mm"];
    if (doc["debounce_ms"].is<uint32_t>())      s->debounce_ms         = doc["debounce_ms"];
    if (doc["pick_current_a"].is<float>())      s->pick_current_a      = doc["pick_current_a"];
    if (doc["hold_current_a"].is<float>())      s->hold_current_a      = doc["hold_current_a"];
    if (doc["encoder_source"].is<uint8_t>())    s->encoder_source      = doc["encoder_source"];

    if (s->pulses_per_mm   <= 0.0f) { return makeResult(false, "set_config","bad_pulses_per_mm"); }
    if (s->pick_current_a  <= 0.0f) { return makeResult(false, "set_config","bad_pick_current"); }
    if (s->hold_current_a  <= 0.0f) { return makeResult(false, "set_config","bad_hold_current"); }
    if (s->hold_current_a  >= s->pick_current_a) {
        return makeResult(false, "set_config","hold_ge_pick"); }
    if (s->encoder_source  > 1) { return makeResult(false, "set_config","bad_encoder_source"); }

    cfg::Config::publish();
    rt::onConfigApplied();
    prog::markDirty();
    return makeResult(true, "set_config");
}

static Result handleSetPattern(JsonDocument& doc) {
    if (!doc["gun"].is<uint8_t>()) { return makeResult(false, "set_pattern","missing_gun"); }
    uint8_t gunOneBased = doc["gun"].as<uint8_t>();
    if (gunOneBased < 1 || gunOneBased > pins::NUM_GUNS) {
        return makeResult(false, "set_pattern","invalid_gun");
    }
    const char* typeStr = doc["type"] | "";
    cfg::PatternType type;
    if      (!strcmp(typeStr, "lines")) type = cfg::PatternType::Lines;
    else if (!strcmp(typeStr, "dots"))  type = cfg::PatternType::Dots;
    else if (!strcmp(typeStr, "none"))  type = cfg::PatternType::None;
    else { return makeResult(false, "set_pattern","bad_type"); }

    cfg::RuntimeConfig* s = cfg::Config::editScratch();
    cfg::GunPattern&    gp = s->pattern[gunOneBased - 1];

    if (type == cfg::PatternType::None) {
        gp.type  = cfg::PatternType::None;
        gp.count = 0;
    } else {
        JsonArrayConst arr = doc["elements"].as<JsonArrayConst>();
        if (arr.isNull()) { return makeResult(false, "set_pattern","missing_elements"); }

        const char* reason = nullptr;
        if (!parsePatternElements(arr, type, gp, reason)) {
            return makeResult(false, "set_pattern", reason ? reason : "parse_error");
        }
    }
    if (doc["on_timeout_ms"].is<float>()) {
        float ot = doc["on_timeout_ms"].as<float>();
        if (ot <= 0.0f) { return makeResult(false, "set_pattern","bad_on_timeout"); }
        gp.on_timeout_ms = ot;
    }
    cfg::Config::publish();
    prog::markDirty();
    return makeResult(true, "set_pattern");
}

static Result handleCalibArm(JsonDocument& doc) {
    if (!doc["paper_length_mm"].is<float>()) {
        return makeResult(false, "calib_arm","missing_paper_length");
    }
    float L = doc["paper_length_mm"].as<float>();
    if (L <= 0.0f) { return makeResult(false, "calib_arm","bad_paper_length"); }
    rt::onCalibArm(L);
    return makeResult(true, "calib_arm");
}

static Result handleTestOpen(JsonDocument& doc) {
    if (!doc["gun"].is<uint8_t>()) { return makeResult(false, "test_open","missing_gun"); }
    uint8_t  gun     = doc["gun"].as<uint8_t>();
    uint32_t timeout = doc["timeout_ms"] | 1000u;
    if (gun > pins::NUM_GUNS)        { return makeResult(false, "test_open","invalid_gun"); }
    if (timeout == 0 || timeout > 10000u) {
        return makeResult(false, "test_open","bad_timeout");
    }
    if (cfg::g_sys.fault.load())     { return makeResult(false, "test_open","hardware_fault"); }
    rt::onTestOpen(gun, timeout);
    return makeResult(true, "test_open");
}

static Result handleTestClose(JsonDocument& doc) {
    uint8_t gun = doc["gun"] | 0;
    if (gun > pins::NUM_GUNS) { return makeResult(false, "test_close","invalid_gun"); }
    rt::onTestClose(gun);
    return makeResult(true, "test_close");
}

static Result handlePing(JsonDocument&) {
    return makeResult(true, "ping");
}

static Result handleSwTrigger(JsonDocument&) {
    if (cfg::g_sys.fault.load()) { return makeResult(false, "sw_trigger","hardware_fault"); }
    rt::onSwTrigger();
    return makeResult(true, "sw_trigger");
}

Result dispatch(const char* line, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line, len);
    if (err) { return makeResult(false, "", "bad_json"); }

    const char* command = doc["cmd"] | "";
    feedWatchdog();

    if      (!strcmp(command, "set_active"))  return handleSetActive(doc);
    else if (!strcmp(command, "set_config"))  return handleSetConfig(doc);
    else if (!strcmp(command, "set_pattern")) return handleSetPattern(doc);
    else if (!strcmp(command, "calib_arm"))   return handleCalibArm(doc);
    else if (!strcmp(command, "test_open"))   return handleTestOpen(doc);
    else if (!strcmp(command, "test_close"))  return handleTestClose(doc);
    else if (!strcmp(command, "ping"))        return handlePing(doc);
    else if (!strcmp(command, "sw_trigger"))  return handleSwTrigger(doc);
    else                                      return makeResult(false, command, "unknown_cmd");
}

} // namespace cmd
