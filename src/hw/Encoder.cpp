#include "Encoder.h"
#include "Pins.h"
#include "config/Config.h"

#include <driver/pcnt.h>
#include <driver/gpio.h>
#include <esp_timer.h>

// Forward decls from pattern scheduler (Core 1).
namespace pattern {
    void onPhotocellEdge       (uint32_t pulseCountAtEdge) IRAM_ATTR;
    void onPhotocellFallingEdge(uint32_t pulseCountAtEdge) IRAM_ATTR;
}

namespace encoder {

// Both encoders are counted in hardware in parallel, each on its own PCNT
// unit -- there is no wiring/resource conflict.  cfg::RuntimeConfig::
// encoder_source only selects which unit's count pulseCount() reports to the
// pattern scheduler, so switching encoders is a pure config change (no
// reflash, no rewiring).
//   Source 0 (primary) -> PCNT_UNIT_0, pins::ENCODER      (fast 5V, GPIO40)
//   Source 1 (alt)     -> PCNT_UNIT_1, pins::ENCODER_ALT  (24V opto, GPIO5)
static constexpr pcnt_unit_t PCNT_UNIT_PRIMARY = PCNT_UNIT_0;
static constexpr pcnt_unit_t PCNT_UNIT_ALT     = PCNT_UNIT_1;
static constexpr int16_t     PCNT_LIMIT        = 30000;   // wrap window

// 32-bit accumulators extending each PCNT's 16-bit counter.  Indexed by
// source (0 = primary, 1 = alt).
static volatile uint32_t s_pulseAccum[2] = {0, 0};

// Debounce state for photocell.
static volatile int64_t s_lastEdgeUs = 0;
static bool s_pcntIsrInstalled = false;

static void IRAM_ATTR pcntOverflowIsr(void* arg) {
    uint8_t     src  = (uint8_t)(uintptr_t)arg;
    pcnt_unit_t unit = (src == 0) ? PCNT_UNIT_PRIMARY : PCNT_UNIT_ALT;
    uint32_t    status = 0;
    pcnt_get_event_status(unit, &status);
    if (status & PCNT_EVT_H_LIM) {
        s_pulseAccum[src] += (uint32_t)PCNT_LIMIT;
        pcnt_counter_clear(unit);
    }
}

static inline uint32_t IRAM_ATTR readUnit(pcnt_unit_t unit, uint8_t src) {
    int16_t cnt = 0;
    pcnt_get_counter_value(unit, &cnt);
    return s_pulseAccum[src] + (uint32_t)(uint16_t)cnt;
}

uint32_t IRAM_ATTR pulseCount() {
    uint8_t src = cfg::Config::active()->encoder_source;
    return (src == 0) ? readUnit(PCNT_UNIT_PRIMARY, 0)
                       : readUnit(PCNT_UNIT_ALT,     1);
}

static void IRAM_ATTR photocellIsr(void* /*arg*/) {
    int64_t  now        = esp_timer_get_time();
    uint32_t debounceUs = cfg::Config::active()->debounce_ms * 1000u;
    if ((uint64_t)(now - s_lastEdgeUs) < debounceUs) return;
    s_lastEdgeUs = now;

    uint32_t p     = pulseCount();
    int      level = gpio_get_level((gpio_num_t)pins::PHOTOCELL);
    if (level) pattern::onPhotocellEdge(p);
    else       pattern::onPhotocellFallingEdge(p);
}

static void initPcnt(pcnt_unit_t unit, int8_t gpioPin, uint8_t srcIdx) {
    pcnt_config_t c = {};
    c.pulse_gpio_num = gpioPin;
    c.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
    c.lctrl_mode     = PCNT_MODE_KEEP;
    c.hctrl_mode     = PCNT_MODE_KEEP;
    c.pos_mode       = PCNT_COUNT_INC;        // count rising edges
    c.neg_mode       = PCNT_COUNT_DIS;
    c.counter_h_lim  = PCNT_LIMIT;
    c.counter_l_lim  = -1;
    c.unit           = unit;
    c.channel        = PCNT_CHANNEL_0;
    pcnt_unit_config(&c);
    if (!s_pcntIsrInstalled) {
        esp_err_t err = pcnt_isr_service_install(ESP_INTR_FLAG_IRAM);
        s_pcntIsrInstalled = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    }

    // ~80 MHz APB / 1023 ticks ~= 12.8 us min pulse width filter.  Plenty for
    // an industrial encoder; tightens if needed via cfg later.
    pcnt_set_filter_value(unit, 100);
    pcnt_filter_enable(unit);

    pcnt_event_enable(unit, PCNT_EVT_H_LIM);
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);

    pcnt_isr_handler_add(unit, pcntOverflowIsr, (void*)(uintptr_t)srcIdx);
    pcnt_counter_resume(unit);
}

static void initPhotocell() {
    gpio_config_t g = {};
    g.pin_bit_mask = (1ULL << pins::PHOTOCELL);
    g.mode         = GPIO_MODE_INPUT;
    g.pull_up_en   = GPIO_PULLUP_DISABLE;     // opto provides defined level
    g.pull_down_en = GPIO_PULLDOWN_DISABLE;
    g.intr_type    = GPIO_INTR_ANYEDGE;       // both edges; level decides
    gpio_config(&g);

    // gpio_install_isr_service is idempotent-ish: ESP_ERR_INVALID_STATE if
    // already installed (e.g. by Arduino attachInterrupt).  Ignore that.
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add((gpio_num_t)pins::PHOTOCELL, photocellIsr, nullptr);
}

void init() {
    initPcnt(PCNT_UNIT_PRIMARY, pins::ENCODER,     0);
    initPcnt(PCNT_UNIT_ALT,     pins::ENCODER_ALT, 1);
    initPhotocell();
}

void injectSwTrigger() {
    pattern::onPhotocellEdge(pulseCount());
}

} // namespace encoder
