#include "Driver.h"
#include <driver/gpio.h>
#include <soc/gpio_struct.h>

namespace drv {

// The IRAM setters below run from ISRs (faultIsr -> killAll, abort -> setIn1)
// that can fire while Core 0 writes flash.  gpio_set_level() lives in flash,
// and the compiler emitted gpio_ll_set_level() out of line in flash too, so
// write the W1TS/W1TC registers directly (same as gpio_ll_set_level).
static inline __attribute__((always_inline)) void pinWrite(int8_t pin, bool high) {
    if (pin < 32) {
        if (high) GPIO.out_w1ts = (1u << pin);
        else      GPIO.out_w1tc = (1u << pin);
    } else {
        if (high) GPIO.out1_w1ts.data = (1u << (pin - 32));
        else      GPIO.out1_w1tc.data = (1u << (pin - 32));
    }
}

void init() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        gpio_num_t in1 = (gpio_num_t)pins::DRV_IN1[g];
        gpio_num_t in2 = (gpio_num_t)pins::MUX_IN2[g];
        gpio_num_t sel = (gpio_num_t)pins::MUX_SELECT[g];

        gpio_reset_pin(in1);
        gpio_reset_pin(in2);
        gpio_reset_pin(sel);

        gpio_set_direction(in1, GPIO_MODE_OUTPUT);
        gpio_set_direction(in2, GPIO_MODE_OUTPUT);
        gpio_set_direction(sel, GPIO_MODE_OUTPUT);

        gpio_set_level(in1, 0);
        gpio_set_level(in2, 0);
        gpio_set_level(sel, 0);   // ESP32-controlled IN2 by default -> coast
    }
}

void IRAM_ATTR setIn1(uint8_t g, bool high) {
    pinWrite(pins::DRV_IN1[g], high);
}

void IRAM_ATTR setMuxIn2(uint8_t g, bool high) {
    pinWrite(pins::MUX_IN2[g], high);
}

void IRAM_ATTR setMuxSelect(uint8_t g, bool hwLoop) {
    pinWrite(pins::MUX_SELECT[g], hwLoop);
}

void IRAM_ATTR killAll() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        pinWrite(pins::DRV_IN1[g],    false);
        pinWrite(pins::MUX_IN2[g],    false);
        pinWrite(pins::MUX_SELECT[g], false);  // coast under ESP32 control
    }
}

} // namespace drv
