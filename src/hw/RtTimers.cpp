#include "RtTimers.h"

#include <esp_intr_alloc.h>
#include <driver/periph_ctrl.h>
#include <soc/periph_defs.h>
#include <soc/timer_group_struct.h>

namespace rttimer {

// APB 80 MHz / 80 = 1 MHz: one counter tick per microsecond.
static constexpr uint32_t DIVIDER      = 80;
// Smallest alarm distance: the counter must not pass the alarm value while
// it is being written.
static constexpr uint32_t MIN_DELAY_US = 3;

static IsrFn         s_onClose   = nullptr;
static IsrFn         s_onTick    = nullptr;
static intr_handle_t s_closeIntr = nullptr;
static intr_handle_t s_tickIntr  = nullptr;

// Register access only: the timer driver functions live in flash, and these
// paths run in interrupts that must survive a flash write.
static inline void IRAM_ATTR restartCounter(volatile timg_hwtimer_reg_t& t, uint32_t alarmUs) {
    t.config.tn_alarm_en  = 0;
    t.loadhi.tn_load_hi   = 0;
    t.loadlo.tn_load_lo   = 0;
    t.load.val            = 1;          // counter := 0
    t.alarmhi.tn_alarm_hi = 0;
    t.alarmlo.tn_alarm_lo = alarmUs;
    t.config.tn_alarm_en  = 1;
}

static void IRAM_ATTR closeIsr(void*) {
    TIMERG0.int_clr_timers.val = BIT(0);
    if (s_onClose) s_onClose();
}

static void IRAM_ATTR tickIsr(void*) {
    TIMERG1.int_clr_timers.val = BIT(0);
    // Re-arm before the work, so nothing below can stop the tick.
    restartCounter(TIMERG1.hw_timer[0], SUPERVISOR_PERIOD_US);
    if (s_onTick) s_onTick();
}

void IRAM_ATTR closeAlarmIn(uint32_t delayUs) {
    restartCounter(TIMERG0.hw_timer[0], delayUs < MIN_DELAY_US ? MIN_DELAY_US : delayUs);
}

void IRAM_ATTR closeAlarmOff() {
    TIMERG0.hw_timer[0].config.tn_alarm_en = 0;
}

void IRAM_ATTR supervisorRestart() {
    restartCounter(TIMERG1.hw_timer[0], SUPERVISOR_PERIOD_US);
}

static void configure(volatile timg_hwtimer_reg_t& t) {
    t.config.tn_en         = 0;
    t.config.tn_alarm_en   = 0;
    t.config.tn_use_xtal   = 0;         // APB clock
    t.config.tn_divider    = DIVIDER;
    t.config.tn_increase   = 1;
    t.config.tn_autoreload = 0;         // re-armed explicitly in software
    t.loadhi.tn_load_hi    = 0;
    t.loadlo.tn_load_lo    = 0;
    t.load.val             = 1;
    t.config.tn_en         = 1;
}

bool init(IsrFn onCloseAlarm, IsrFn onSupervisorTick) {
    s_onClose = onCloseAlarm;
    s_onTick  = onSupervisorTick;

    periph_module_enable(PERIPH_TIMG0_MODULE);
    periph_module_enable(PERIPH_TIMG1_MODULE);
    configure(TIMERG0.hw_timer[0]);
    configure(TIMERG1.hw_timer[0]);
    TIMERG0.int_clr_timers.val = BIT(0);
    TIMERG1.int_clr_timers.val = BIT(0);

    // esp_intr_alloc binds each interrupt to the calling core.
    esp_err_t e1 = esp_intr_alloc(ETS_TG0_T0_LEVEL_INTR_SOURCE, ESP_INTR_FLAG_IRAM,
                                  closeIsr, nullptr, &s_closeIntr);
    esp_err_t e2 = esp_intr_alloc(ETS_TG1_T0_LEVEL_INTR_SOURCE, ESP_INTR_FLAG_IRAM,
                                  tickIsr, nullptr, &s_tickIntr);
    TIMERG0.int_ena_timers.val |= BIT(0);
    TIMERG1.int_ena_timers.val |= BIT(0);

    supervisorRestart();
    return e1 == ESP_OK && e2 == ESP_OK;
}

} // namespace rttimer
