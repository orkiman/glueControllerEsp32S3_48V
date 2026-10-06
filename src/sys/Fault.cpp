#include "Fault.h"
#include "hw/Pins.h"
#include "hw/Driver.h"
#include "rt/GunSequencer.h"
#include "config/Config.h"
#include "comms/Events.h"

#include <driver/gpio.h>
#include <string.h>

namespace fault {

// String literals live in flash .rodata; the ISR copies from this DRAM copy.
static DRAM_ATTR const char kHardwareFault[] = "hardware_fault";

static void IRAM_ATTR faultIsr(void*) {
    // Master kill: coast every gun.  All ISRs that try to fire later will see
    // g_sys.fault == true and refuse.
    drv::killAll();
    seq::abortAll();
    cfg::g_sys.fault .store(true,  std::memory_order_release);
    cfg::g_sys.active.store(false, std::memory_order_release);

    evt::Event e{}; e.kind = evt::Kind::Error;     // e.cmd = "" (zeroed)
    strncpy(e.reason, kHardwareFault, sizeof(e.reason) - 1);

    BaseType_t hp = pdFALSE;
    evt::postFromISR(e, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void init() {
    gpio_config_t g = {};
    g.pin_bit_mask = (1ULL << pins::N_FAULT);
    g.mode         = GPIO_MODE_INPUT;
    g.pull_up_en   = GPIO_PULLUP_DISABLE;     // external 10k pull-up
    g.pull_down_en = GPIO_PULLDOWN_DISABLE;
    g.intr_type    = GPIO_INTR_NEGEDGE;       // active LOW
    gpio_config(&g);
    gpio_isr_handler_add((gpio_num_t)pins::N_FAULT, faultIsr, nullptr);
}

} // namespace fault
