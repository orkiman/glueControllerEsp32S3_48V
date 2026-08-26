#include "Status.h"
#include "rt/PatternScheduler.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace status {

static void statusTask(void*) {
    for (;;) {
        pattern::Metrics metrics = pattern::metrics();
        evt::postStatus(pattern::currentPosMm(),
                        pattern::currentSpeedMmS(),
                        cfg::g_sys.active.load(std::memory_order_acquire),
                        metrics.max_loop_gap_us,
                        metrics.max_event_late_pulses,
                        metrics.pattern_events,
                        metrics.sheet_queue_overflows);
        vTaskDelay(pdMS_TO_TICKS(200));   // 5 Hz
    }
}

void init() {
    xTaskCreatePinnedToCore(statusTask, "status", 4096, nullptr, 2, nullptr, 0);
}

} // namespace status
