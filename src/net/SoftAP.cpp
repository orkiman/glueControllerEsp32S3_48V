#include "SoftAP.h"
#include "WebPage.h"
#include "comms/CommandDispatcher.h"
#include "comms/ControlAuthority.h"
#include "comms/Events.h"
#include "config/Config.h"
#include "hw/Pins.h"
#include "rt/Control.h"
#include "rt/PatternScheduler.h"
#include "storage/ProgramStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace net {

static DNSServer s_dns;
static WebServer s_server(80);
static char s_ssid[24];

static volatile uint32_t s_calibBits = 0;
static volatile bool     s_calibReady = false;
static TaskHandle_t      s_netTask = nullptr;

static void IRAM_ATTR onNetEvent(const evt::Event& e, void*) {
    if (e.kind != evt::Kind::CalibResult) return;
    uint32_t bits;
    memcpy(&bits, &e.f1, sizeof(bits));   // bit-copy float, no FPU in ISR
    s_calibBits = bits;
    s_calibReady = true;
    if (!s_netTask) return;
    if (xPortInIsrContext()) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_netTask, &hp);
    } else {
        xTaskNotifyGive(s_netTask);
    }
}

static void sendIndex() {
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/html; charset=utf-8", web::INDEX_HTML);
}

static void sendResult(int status, bool ok, const char* command = "",
                       const char* reason = "") {
    JsonDocument doc;
    doc["ok"] = ok;
    if (command[0]) doc["cmd"] = command;
    if (reason[0]) doc["reason"] = reason;
    String body;
    serializeJson(doc, body);
    s_server.send(status, "application/json", body);
}

static void sendStatus() {
    pattern::Metrics metrics = pattern::metrics();
    JsonDocument doc;
    doc["active"] = cfg::g_sys.active.load(std::memory_order_acquire);
    doc["fault"] = cfg::g_sys.fault.load(std::memory_order_acquire);
    doc["pos_mm"] = pattern::currentPosMm();
    doc["speed_mm_s"] = pattern::currentSpeedMmS();
    doc["control_owner"] = control::ownerName();
    doc["max_loop_gap_us"] = metrics.max_loop_gap_us;
    doc["max_event_late_pulses"] = metrics.max_event_late_pulses;
    doc["pattern_events"] = metrics.pattern_events;
    doc["sheet_queue_overflows"] = metrics.sheet_queue_overflows;
    String body;
    serializeJson(doc, body);
    s_server.send(200, "application/json", body);
}

static void sendConfig() {
    const cfg::RuntimeConfig* config = cfg::Config::active();
    JsonDocument doc;
    doc["pulses_per_mm"] = config->pulses_per_mm;
    doc["min_speed_mm_s"] = config->min_speed_mm_s;
    doc["photocell_offset_mm"] = config->photocell_offset_mm;
    doc["debounce_ms"] = config->debounce_ms;
    doc["pick_current_a"] = config->pick_current_a;
    doc["hold_current_a"] = config->hold_current_a;
    doc["encoder_source"] = config->encoder_source;
    String body;
    serializeJson(doc, body);
    s_server.send(200, "application/json", body);
}

static void sendPattern() {
    if (!s_server.hasArg("gun")) {
        sendResult(400, false, "", "missing_gun");
        return;
    }
    int gun = s_server.arg("gun").toInt();
    if (gun < 1 || gun > pins::NUM_GUNS) {
        sendResult(400, false, "", "invalid_gun");
        return;
    }
    const cfg::GunPattern& pattern = cfg::Config::active()->pattern[gun - 1];
    JsonDocument doc;
    doc["gun"] = gun;
    doc["type"] = pattern.type == cfg::PatternType::Lines ? "lines" :
                  pattern.type == cfg::PatternType::Dots ? "dots" : "none";
    doc["on_timeout_ms"] = pattern.on_timeout_ms;
    JsonArray elements = doc["elements"].to<JsonArray>();
    for (uint8_t i = 0; i < pattern.count; ++i) {
        JsonObject element = elements.add<JsonObject>();
        element["start"] = pattern.elems[i].start_mm;
        element["end"] = pattern.elems[i].end_mm;
        if (pattern.type == cfg::PatternType::Dots) {
            element["spacing"] = pattern.elems[i].spacing_mm;
        }
    }
    String body;
    serializeJson(doc, body);
    s_server.send(200, "application/json", body);
}

static bool parseProgramRequest(JsonDocument& doc) {
    String body = s_server.arg("plain");
    if (body.length() == 0 || body.length() > 256) return false;
    return !deserializeJson(doc, body);
}

static bool canManagePrograms() {
    if (!control::canUse(control::Owner::Web)) {
        sendResult(423, false, "program", "control_busy");
        return false;
    }
    if (cfg::g_sys.active.load(std::memory_order_acquire)) {
        sendResult(409, false, "program", "active");
        return false;
    }
    return true;
}

static void sendPrograms() {
    prog::ProgramMeta programs[prog::MAX_PROGRAMS];
    size_t count = 0;
    if (!prog::list(programs, prog::MAX_PROGRAMS, count)) {
        sendResult(500, false, "program_list", "storage_error");
        return;
    }
    JsonDocument doc;
    doc["active_id"] = prog::activeId();
    JsonArray items = doc["programs"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        JsonObject item = items.add<JsonObject>();
        item["id"] = programs[i].id;
        item["name"] = programs[i].name;
    }
    String body;
    serializeJson(doc, body);
    s_server.send(200, "application/json", body);
}

static void handleProgramSave() {
    if (!canManagePrograms()) return;
    JsonDocument doc;
    if (!parseProgramRequest(doc)) {
        sendResult(400, false, "program_save", "bad_json");
        return;
    }
    uint8_t id = doc["id"] | 0;
    const char* name = doc["name"] | "";
    size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > prog::MAX_NAME_LEN) {
        sendResult(400, false, "program_save", "bad_name");
        return;
    }
    uint8_t savedId = 0;
    bool ok = prog::save(id, name, &savedId);
    if (!ok) {
        sendResult(409, false, "program_save", "save_failed");
        return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["id"] = savedId;
    response["name"] = name;
    String body;
    serializeJson(response, body);
    s_server.send(200, "application/json", body);
}

static void handleProgramLoad() {
    if (!canManagePrograms()) return;
    JsonDocument doc;
    if (!parseProgramRequest(doc) || !doc["id"].is<uint8_t>()) {
        sendResult(400, false, "program_load", "bad_request");
        return;
    }
    bool ok = prog::load(doc["id"].as<uint8_t>());
    sendResult(ok ? 200 : 404, ok, "program_load", ok ? "" : "load_failed");
}

static void handleProgramRename() {
    if (!canManagePrograms()) return;
    JsonDocument doc;
    if (!parseProgramRequest(doc) || !doc["id"].is<uint8_t>()) {
        sendResult(400, false, "program_rename", "bad_request");
        return;
    }
    const char* name = doc["name"] | "";
    size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > prog::MAX_NAME_LEN) {
        sendResult(400, false, "program_rename", "bad_name");
        return;
    }
    bool ok = prog::rename(doc["id"].as<uint8_t>(), name);
    sendResult(ok ? 200 : 404, ok, "program_rename", ok ? "" : "rename_failed");
}

static void handleProgramDelete() {
    if (!canManagePrograms()) return;
    JsonDocument doc;
    if (!parseProgramRequest(doc) || !doc["id"].is<uint8_t>()) {
        sendResult(400, false, "program_delete", "bad_request");
        return;
    }
    bool ok = prog::erase(doc["id"].as<uint8_t>());
    sendResult(ok ? 200 : 404, ok, "program_delete", ok ? "" : "delete_failed");
}

static void handleCommand() {
    if (!control::canUse(control::Owner::Web)) {
        sendResult(423, false, "", "control_busy");
        return;
    }
    String body = s_server.arg("plain");
    if (body.length() == 0 || body.length() >= 1024) {
        sendResult(400, false, "", "bad_length");
        return;
    }
    cmd::Result result = cmd::dispatch(body.c_str(), body.length());
    sendResult(result.ok ? 200 : 400, result.ok, result.cmd, result.reason);
}

static void handleCalib() {
    if (!control::canUse(control::Owner::Web)) {
        sendResult(423, false, "calib", "control_busy");
        return;
    }
    String body = s_server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["paper_length_mm"].is<float>()) {
        sendResult(400, false, "calib", "bad_request");
        return;
    }
    float L = doc["paper_length_mm"].as<float>();
    if (L <= 0.0f) {
        sendResult(400, false, "calib", "bad_paper_length");
        return;
    }
    s_calibReady = false;
    s_calibBits = 0;
    rt::onCalibArm(L);

    bool got = false;
    for (int i = 0; i < 150; ++i) {            // up to 15 s
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) != 0) {
            if (s_calibReady) { got = true; break; }
        }
        if (s_calibReady) { got = true; break; }
        s_dns.processNextRequest();
        s_server.handleClient();
    }
    if (!got) {
        sendResult(408, false, "calib", "timeout");
        return;
    }
    float ppm;
    uint32_t bits = s_calibBits;
    memcpy(&ppm, &bits, sizeof(bits));

    JsonDocument resp;
    resp["ok"] = true;
    resp["pulses_per_mm"] = ppm;
    String out;
    serializeJson(resp, out);
    s_server.send(200, "application/json", out);
}

static void networkTask(void*) {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(s_ssid, sizeof(s_ssid), "GlueController-%04X", (uint16_t)mac);

    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    if (!WiFi.softAP(s_ssid)) {
        evt::postError("wifi", "softap_failed");
        vTaskDelete(nullptr);
        return;
    }
    evt::postAck("wifi_ready");

    s_dns.start(53, "*", WiFi.softAPIP());
    s_server.on("/", HTTP_GET, sendIndex);
    s_server.on("/generate_204", HTTP_GET, sendIndex);
    s_server.on("/hotspot-detect.html", HTTP_GET, sendIndex);
    s_server.on("/connecttest.txt", HTTP_GET, sendIndex);
    s_server.on("/ncsi.txt", HTTP_GET, sendIndex);
    s_server.on("/health", HTTP_GET, [] {
        s_server.send(200, "application/json", "{\"ok\":true}");
    });
    s_server.on("/api/status", HTTP_GET, sendStatus);
    s_server.on("/api/config", HTTP_GET, sendConfig);
    s_server.on("/api/pattern", HTTP_GET, sendPattern);
    s_server.on("/api/programs", HTTP_GET, sendPrograms);
    s_server.on("/api/program/save", HTTP_POST, handleProgramSave);
    s_server.on("/api/program/load", HTTP_POST, handleProgramLoad);
    s_server.on("/api/program/rename", HTTP_POST, handleProgramRename);
    s_server.on("/api/program/delete", HTTP_POST, handleProgramDelete);
    s_server.on("/api/control/acquire", HTTP_POST, [] {
        sendResult(control::acquireWeb() ? 200 : 409,
                   control::canUse(control::Owner::Web), "control_acquire",
                   control::canUse(control::Owner::Web) ? "" : "active");
    });
    s_server.on("/api/control/heartbeat", HTTP_POST, [] {
        bool ok = control::heartbeatWeb();
        sendResult(ok ? 200 : 409, ok, "heartbeat", ok ? "" : "not_owner");
    });
    s_server.on("/api/control/release", HTTP_POST, [] {
        control::releaseWeb();
        sendResult(200, true, "control_release");
    });
    s_server.on("/api/command", HTTP_POST, handleCommand);
    s_server.on("/api/calibrate", HTTP_POST, handleCalib);
    s_server.onNotFound(sendIndex);
    s_server.begin();

    s_netTask = xTaskGetCurrentTaskHandle();
    evt::setCallback(onNetEvent, nullptr);

    for (;;) {
        s_dns.processNextRequest();
        s_server.handleClient();
        prog::service();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void init() {
    xTaskCreatePinnedToCore(networkTask, "network", 8192, nullptr, 1, nullptr, 0);
}

} // namespace net
