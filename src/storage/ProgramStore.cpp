#include "ProgramStore.h"
#include "config/Config.h"
#include "hw/Pins.h"
#include "rt/Control.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace prog {

namespace {

constexpr const char* kIndexPath   = "/programs/index.json";
constexpr const char* kProgramPath = "/programs/prog_%u.json";
constexpr const char* kSeedName    = "ברירת מחדל";
constexpr const char* kFallbackName = "Default";

struct Index {
    uint8_t     activeId = 0;
    ProgramMeta programs[MAX_PROGRAMS];
    size_t      count = 0;
};

Index               s_index;
uint8_t             s_activeId = 0;
char                s_activeName[MAX_NAME_LEN + 1] = {};
bool                s_mounted = false;
bool                s_runtimeReady = false;
bool                s_dirty = false;
uint32_t            s_dirtySinceMs = 0;
SemaphoreHandle_t   s_mtx = nullptr;

class MutexLock {
public:
    explicit MutexLock(SemaphoreHandle_t mtx) : mtx_(mtx) {
        if (mtx_) xSemaphoreTake(mtx_, portMAX_DELAY);
    }
    ~MutexLock() { if (mtx_) xSemaphoreGive(mtx_); }
private:
    SemaphoreHandle_t mtx_;
};

static inline void setStr(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline String programPath(uint8_t id) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), kProgramPath, (unsigned)id);
    return String(buf);
}

static inline String tempPath(const String& base) {
    return base + String(".tmp");
}

static bool writeFileAtomic(const String& path, const String& content) {
    String tmp = tempPath(path);
    File f = SPIFFS.open(tmp, "w");
    if (!f) return false;
    size_t written = f.write((const uint8_t*)content.c_str(), content.length());
    f.close();
    if (written != content.length()) {
        SPIFFS.remove(tmp);
        return false;
    }
    if (SPIFFS.exists(path)) SPIFFS.remove(path);
    return SPIFFS.rename(tmp, path);
}

static int findIndexIndex(uint8_t id) {
    for (size_t i = 0; i < s_index.count; ++i) {
        if (s_index.programs[i].id == id) return (int)i;
    }
    return -1;
}

static ProgramMeta* findIndexEntry(uint8_t id) {
    int i = findIndexIndex(id);
    return (i >= 0) ? &s_index.programs[i] : nullptr;
}

static uint8_t findFreeId() {
    for (uint8_t id = 1; id <= MAX_PROGRAMS; ++id) {
        if (!findIndexEntry(id)) return id;
    }
    return 0;
}

static void insertSorted(uint8_t id, const char* name) {
    size_t pos = s_index.count;
    for (size_t i = 0; i < s_index.count; ++i) {
        if (id < s_index.programs[i].id) { pos = i; break; }
    }
    for (size_t i = s_index.count; i > pos; --i) {
        s_index.programs[i] = s_index.programs[i - 1];
    }
    s_index.programs[pos].id = id;
    setStr(s_index.programs[pos].name, sizeof(s_index.programs[pos].name), name);
    if (s_index.count < MAX_PROGRAMS) ++s_index.count;
}

static bool writeIndexFile() {
    JsonDocument doc;
    doc["activeId"] = s_activeId;
    JsonArray arr = doc["programs"].to<JsonArray>();
    for (size_t i = 0; i < s_index.count; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]   = s_index.programs[i].id;
        o["name"] = s_index.programs[i].name;
    }
    String body;
    size_t n = serializeJson(doc, body);
    (void)n;
    return writeFileAtomic(kIndexPath, body);
}

static bool readIndexFile() {
    s_index.count = 0;
    s_index.activeId = 0;
    File f = SPIFFS.open(kIndexPath, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    JsonArrayConst arr = doc["programs"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    s_index.activeId = doc["activeId"] | 0;
    s_index.count = 0;
    for (JsonObjectConst o : arr) {
        if (!o["id"].is<uint8_t>() || !o["name"].is<const char*>()) continue;
        uint8_t id = o["id"];
        if (id < 1 || id > MAX_PROGRAMS) continue;
        if (s_index.count >= MAX_PROGRAMS) break;
        s_index.programs[s_index.count].id = id;
        setStr(s_index.programs[s_index.count].name,
               sizeof(s_index.programs[s_index.count].name),
               o["name"]);
        ++s_index.count;
    }
    return true;
}

static bool writeProgramFile(uint8_t id, const cfg::RuntimeConfig* config, const char* name) {
    JsonDocument doc;
    doc["name"] = name;

    JsonObject cfgObj = doc["config"].to<JsonObject>();
    cfgObj["pulses_per_mm"]       = config->pulses_per_mm;
    cfgObj["min_speed_mm_s"]      = config->min_speed_mm_s;
    cfgObj["photocell_offset_mm"] = config->photocell_offset_mm;
    cfgObj["debounce_ms"]         = config->debounce_ms;
    cfgObj["pick_current_a"]      = config->pick_current_a;
    cfgObj["hold_current_a"]      = config->hold_current_a;
    cfgObj["encoder_source"]      = config->encoder_source;

    JsonArray patterns = doc["patterns"].to<JsonArray>();
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        const cfg::GunPattern& gp = config->pattern[g];
        JsonObject p = patterns.add<JsonObject>();
        const char* typeStr = (gp.type == cfg::PatternType::Lines) ? "lines" :
                              (gp.type == cfg::PatternType::Dots) ? "dots" : "none";
        p["type"] = typeStr;
        p["on_timeout_ms"] = gp.on_timeout_ms;
        JsonArray elems = p["elements"].to<JsonArray>();
        for (uint8_t i = 0; i < gp.count; ++i) {
            JsonObject el = elems.add<JsonObject>();
            el["start"]   = gp.elems[i].start_mm;
            el["end"]     = gp.elems[i].end_mm;
            if (gp.type == cfg::PatternType::Dots) {
                el["spacing"] = gp.elems[i].spacing_mm;
            }
        }
    }

    String body;
    size_t n = serializeJson(doc, body);
    (void)n;
    return writeFileAtomic(programPath(id), body);
}

static bool validateAndParse(JsonDocument& doc, cfg::RuntimeConfig& out, char* nameBuf, size_t nameCap) {
    const char* name = doc["name"] | "";
    if (!name[0]) return false;
    setStr(nameBuf, nameCap, name);

    JsonObjectConst cfgObj = doc["config"].as<JsonObjectConst>();
    if (cfgObj.isNull()) return false;

    if (!cfgObj["pulses_per_mm"].is<float>())       return false;
    float pulsesPerMm = cfgObj["pulses_per_mm"];
    if (pulsesPerMm <= 0.0f) return false;

    if (!cfgObj["min_speed_mm_s"].is<float>())      return false;
    float minSpeed = cfgObj["min_speed_mm_s"];
    if (minSpeed < 0.0f) return false;

    if (!cfgObj["photocell_offset_mm"].is<float>())  return false;
    float offsetMm = cfgObj["photocell_offset_mm"];
    if (offsetMm < 0.0f) return false;

    if (!cfgObj["debounce_ms"].is<uint32_t>())       return false;
    uint32_t debounceMs = cfgObj["debounce_ms"];

    if (!cfgObj["pick_current_a"].is<float>())        return false;
    float pickA = cfgObj["pick_current_a"];
    if (pickA <= 0.0f) return false;

    if (!cfgObj["hold_current_a"].is<float>())        return false;
    float holdA = cfgObj["hold_current_a"];
    if (holdA <= 0.0f || holdA >= pickA) return false;

    if (!cfgObj["encoder_source"].is<uint8_t>())     return false;
    uint8_t encSrc = cfgObj["encoder_source"];
    if (encSrc > 1u) return false;

    out.pulses_per_mm        = pulsesPerMm;
    out.min_speed_mm_s       = minSpeed;
    out.photocell_offset_mm  = offsetMm;
    out.debounce_ms          = debounceMs;
    out.pick_current_a       = pickA;
    out.hold_current_a       = holdA;
    out.encoder_source       = encSrc;

    JsonArrayConst patterns = doc["patterns"].as<JsonArrayConst>();
    if (patterns.isNull() || patterns.size() != pins::NUM_GUNS) return false;

    uint8_t g = 0;
    for (JsonObjectConst p : patterns) {
        if (p.isNull()) return false;
        if (!p["on_timeout_ms"].is<float>()) return false;
        float onTimeout = p["on_timeout_ms"];
        if (onTimeout <= 0.0f) return false;
        out.pattern[g].on_timeout_ms = onTimeout;

        const char* typeStr = p["type"] | "";
        cfg::PatternType type;
        if      (!std::strcmp(typeStr, "none"))  type = cfg::PatternType::None;
        else if (!std::strcmp(typeStr, "lines")) type = cfg::PatternType::Lines;
        else if (!std::strcmp(typeStr, "dots"))  type = cfg::PatternType::Dots;
        else return false;

        out.pattern[g].type  = type;
        out.pattern[g].count = 0;

        JsonArrayConst elems = p["elements"].as<JsonArrayConst>();
        if (type == cfg::PatternType::None) {
            ++g;
            continue;
        }
        if (elems.isNull() || elems.size() == 0 || elems.size() > cfg::MAX_PATTERN_ELEMENTS_PER_GUN) return false;

        uint8_t count = 0;
        for (JsonObjectConst el : elems) {
            if (el.isNull()) return false;
            if (!el["start"].is<float>() || !el["end"].is<float>()) return false;
            float start = el["start"];
            float end   = el["end"];
            if (end < start) return false;
            float spacing = 0.0f;
            if (type == cfg::PatternType::Dots) {
                if (!el["spacing"].is<float>()) return false;
                spacing = el["spacing"];
                if (spacing <= 0.0f) return false;
            }
            out.pattern[g].elems[count].start_mm   = start;
            out.pattern[g].elems[count].end_mm     = end;
            out.pattern[g].elems[count].spacing_mm = spacing;
            ++count;
        }
        out.pattern[g].count = count;
        ++g;
    }
    return true;
}

static bool loadProgramFromFile(uint8_t id) {
    File f = SPIFFS.open(programPath(id), "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    auto parsed = std::make_unique<cfg::RuntimeConfig>();
    char nameBuf[MAX_NAME_LEN + 1];
    if (!validateAndParse(doc, *parsed, nameBuf, sizeof(nameBuf))) return false;

    cfg::RuntimeConfig* sc = cfg::Config::editScratch();
    *sc = *parsed;
    cfg::Config::publish();

    if (s_runtimeReady) rt::onConfigApplied();

    s_activeId = id;
    setStr(s_activeName, sizeof(s_activeName), nameBuf);
    s_dirty = false;
    return true;
}

static bool createSeedProgram(const char* name, uint8_t forcedId = 1) {
    if (!s_mounted) return false;
    const cfg::RuntimeConfig* config = cfg::Config::active();
    if (!writeProgramFile(forcedId, config, name)) return false;

    s_index.count = 0;
    s_index.programs[0].id = forcedId;
    setStr(s_index.programs[0].name, sizeof(s_index.programs[0].name), name);
    s_index.count = 1;
    s_index.activeId = forcedId;
    s_activeId = forcedId;
    setStr(s_activeName, sizeof(s_activeName), name);
    s_dirty = false;
    return writeIndexFile();
}

static bool updateFileName(uint8_t id, const char* name) {
    File f = SPIFFS.open(programPath(id), "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    doc["name"] = name;
    String body;
    size_t n = serializeJson(doc, body);
    (void)n;
    return writeFileAtomic(programPath(id), body);
}

static bool saveInternalLocked(uint8_t id, const char* name, uint8_t* outId) {
    uint8_t targetId = id;
    if (targetId == 0) {
        targetId = findFreeId();
        if (targetId == 0) return false;
    } else {
        if (!findIndexEntry(targetId)) return false;
    }

    const cfg::RuntimeConfig* config = cfg::Config::active();
    if (!writeProgramFile(targetId, config, name)) return false;

    if (id == 0) {
        insertSorted(targetId, name);
    } else {
        ProgramMeta* e = findIndexEntry(targetId);
        if (e) setStr(e->name, sizeof(e->name), name);
    }

    s_index.activeId = targetId;
    s_activeId = targetId;
    setStr(s_activeName, sizeof(s_activeName), name);
    if (outId) *outId = targetId;
    return writeIndexFile();
}

} // namespace

bool init() {
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return false;

    s_mounted = SPIFFS.begin(true);
    s_index.count = 0;
    s_index.activeId = 0;
    s_activeId = 0;
    s_activeName[0] = '\0';
    s_dirty = false;
    s_runtimeReady = false;

    if (s_mounted) readIndexFile();

    if (!s_mounted || s_index.count == 0) {
        if (s_mounted) createSeedProgram(kSeedName, 1);
        else            setStr(s_activeName, sizeof(s_activeName), kFallbackName);
    }

    if (s_index.activeId == 0 && s_index.count > 0) {
        s_index.activeId = s_index.programs[0].id;
    }

    if (s_index.activeId != 0) {
        bool ok = loadProgramFromFile(s_index.activeId);
        if (!ok) {
            // Keep defaults in memory; do not let a corrupt file stop boot.
            s_activeId = 0;
            s_activeName[0] = '\0';
        }
    }

    ProgramMeta* e = findIndexEntry(s_activeId);
    if (e) setStr(s_activeName, sizeof(s_activeName), e->name);
    else   s_activeName[0] = '\0';

    return s_mounted;
}

void onRuntimeInitialized() {
    MutexLock lock(s_mtx);
    s_runtimeReady = true;
    rt::onConfigApplied();
}

bool list(ProgramMeta* out, size_t maxCount, size_t& outCount) {
    MutexLock lock(s_mtx);
    outCount = 0;
    if (!s_mounted) return false;
    size_t n = std::min(s_index.count, maxCount);
    for (size_t i = 0; i < n; ++i) out[i] = s_index.programs[i];
    outCount = n;
    return true;
}

bool save(uint8_t id, const char* name, uint8_t* outId) {
    MutexLock lock(s_mtx);
    if (!s_mounted) return false;
    if (cfg::g_sys.active.load()) return false;
    if (!name || !name[0]) return false;
    return saveInternalLocked(id, name, outId);
}

bool load(uint8_t id) {
    MutexLock lock(s_mtx);
    if (!s_mounted) return false;
    if (cfg::g_sys.active.load()) return false;
    if (id == 0) return false;
    if (!findIndexEntry(id)) return false;
    if (!loadProgramFromFile(id)) return false;

    s_index.activeId = id;
    ProgramMeta* e = findIndexEntry(id);
    if (e) setStr(s_activeName, sizeof(s_activeName), e->name);
    return writeIndexFile();
}

bool rename(uint8_t id, const char* name) {
    MutexLock lock(s_mtx);
    if (!s_mounted) return false;
    if (cfg::g_sys.active.load()) return false;
    if (id == 0 || !name || !name[0]) return false;
    ProgramMeta* e = findIndexEntry(id);
    if (!e) return false;
    if (!updateFileName(id, name)) return false;
    setStr(e->name, sizeof(e->name), name);
    if (s_activeId == id) setStr(s_activeName, sizeof(s_activeName), name);
    return writeIndexFile();
}

bool erase(uint8_t id) {
    MutexLock lock(s_mtx);
    if (!s_mounted) return false;
    if (cfg::g_sys.active.load()) return false;
    if (id == 0) return false;
    int idx = findIndexIndex(id);
    if (idx < 0) return false;

    SPIFFS.remove(programPath(id));
    for (size_t i = (size_t)idx; i + 1 < s_index.count; ++i) {
        s_index.programs[i] = s_index.programs[i + 1];
    }
    if (s_index.count > 0) --s_index.count;

    if (s_index.activeId == id) {
        if (s_index.count > 0) {
            s_index.activeId = s_index.programs[0].id;
            loadProgramFromFile(s_index.activeId);
        } else {
            createSeedProgram(kFallbackName, 1);
        }
    }
    return writeIndexFile();
}

uint8_t activeId() {
    MutexLock lock(s_mtx);
    return s_activeId;
}

bool activeName(char* buf, size_t cap) {
    MutexLock lock(s_mtx);
    if (cap == 0) return false;
    if (s_activeId == 0) { buf[0] = '\0'; return false; }
    setStr(buf, cap, s_activeName);
    return true;
}

void markDirty() {
    MutexLock lock(s_mtx);
    s_dirty = true;
    s_dirtySinceMs = millis();
}

void service() {
    MutexLock lock(s_mtx);
    if (!s_dirty || !s_mounted) return;
    if ((uint32_t)(millis() - s_dirtySinceMs) < 2000u) return;
    if (cfg::g_sys.active.load()) return;
    if (s_activeId == 0 || s_activeName[0] == '\0') return;
    if (saveInternalLocked(s_activeId, s_activeName, nullptr)) {
        s_dirty = false;
    }
}

} // namespace prog
