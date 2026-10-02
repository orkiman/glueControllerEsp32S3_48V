#include "LiveSync.h"
#include "Events.h"
#include "config/Config.h"
#include "hw/Pins.h"
#include "rt/Control.h"
#include "storage/ProgramStore.h"

#include <atomic>
#include <cstring>
#include <freertos/semphr.h>

namespace livesync {

static SemaphoreHandle_t     s_mtx = nullptr;
static std::atomic<uint32_t> s_rev{1};

static std::atomic<uint32_t> s_calibBits{0};
static std::atomic<bool>     s_calibPending{false};

void init() {
    if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
}

void lock() {
    if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
}

void unlock() {
    if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
}

uint32_t revision() {
    return s_rev.load(std::memory_order_acquire);
}

static inline void bump() {
    s_rev.fetch_add(1, std::memory_order_acq_rel);
}

void configChanged(Source src) {
    bump();
    if (src != Source::Uart) evt::postConfig(cfg::Config::active());
}

void patternChanged(uint8_t gun_1based, Source src) {
    bump();
    if (src != Source::Uart && gun_1based >= 1 && gun_1based <= pins::NUM_GUNS) {
        evt::postPattern(gun_1based, &cfg::Config::active()->pattern[gun_1based - 1]);
    }
}

void emitProgramsList() {
    prog::ProgramMeta list[prog::MAX_PROGRAMS];
    size_t count = 0;
    if (prog::list(list, prog::MAX_PROGRAMS, count)) {
        evt::postProgramsList(list, count, prog::activeId());
    }
}

void programsChanged() {
    bump();
    emitProgramsList();
}

void emitFullState() {
    const cfg::RuntimeConfig* c = cfg::Config::active();
    evt::postConfig(c);
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        evt::postPattern(g + 1, &c->pattern[g]);
    }
    emitProgramsList();
}

void programLoaded() {
    bump();
    emitFullState();
}

void IRAM_ATTR postCalibration(float pulses_per_mm) {
    uint32_t bits;
    std::memcpy(&bits, &pulses_per_mm, sizeof(bits));
    s_calibBits.store(bits, std::memory_order_release);
    s_calibPending.store(true, std::memory_order_release);
}

void service() {
    if (!s_calibPending.exchange(false, std::memory_order_acq_rel)) return;
    uint32_t bits = s_calibBits.load(std::memory_order_acquire);
    float ppm;
    std::memcpy(&ppm, &bits, sizeof(ppm));
    if (!(ppm > 0.0f)) return;

    Guard g;
    cfg::RuntimeConfig* s = cfg::Config::editScratch();
    s->pulses_per_mm = ppm;
    cfg::Config::publish();
    rt::onConfigApplied();
    prog::markDirty();
    configChanged(Source::Internal);
}

} // namespace livesync
