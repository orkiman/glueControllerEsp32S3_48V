#include "Dac.h"
#include "comms/Events.h"

#include <Wire.h>
#include <Adafruit_MCP4728.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace dac {

static constexpr float    VDD            = 3.3f;
static constexpr uint16_t DAC_FULL_SCALE = 4095;

static Adafruit_MCP4728  s_chip;
static TaskHandle_t      s_task         = nullptr;
static std::atomic<bool> s_chipReady{false};
// Serializes chip access so a synchronous blockingSetCode() (called from
// fire() on the RT core) can never collide on the I2C bus with dacTask.
static SemaphoreHandle_t s_i2cMux       = nullptr;

// Target code for each channel.
static std::atomic<uint16_t> s_shadow[pins::NUM_GUNS];

// Codes confirmed on the chip (0xFFFF = unknown).  Written by the I2C writer,
// read from ISRs through isApplied().
static volatile uint16_t s_written[pins::NUM_GUNS] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

static inline uint16_t voltsToCode(float v) {
    if (v <= 0.0f)  return 0;
    if (v >= VDD)   return DAC_FULL_SCALE;
    float code = v * (float)DAC_FULL_SCALE / VDD;
    if (code < 0.0f) code = 0.0f;
    if (code > (float)DAC_FULL_SCALE) code = (float)DAC_FULL_SCALE;
    return (uint16_t)(code + 0.5f);
}

uint16_t codeForVolts(float v) { return voltsToCode(v); }

void IRAM_ATTR setTarget(uint8_t g, uint16_t code) {
    if (g >= pins::NUM_GUNS) return;
    s_shadow[g].store(code, std::memory_order_release);
}

void IRAM_ATTR kick() {
    if (!s_task) return;
    if (xPortInIsrContext()) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        if (hp) portYIELD_FROM_ISR();
    } else {
        xTaskNotifyGive(s_task);
    }
}

bool IRAM_ATTR isApplied(uint8_t g, uint16_t code) {
    if (g >= pins::NUM_GUNS) return false;
    return s_shadow[g].load(std::memory_order_acquire) == code && s_written[g] == code;
}

// Write every channel whose target differs from the chip.  Task context only.
static bool writeAllChannels() {
    if (!s_chipReady.load(std::memory_order_acquire)) return false;

    if (s_i2cMux) xSemaphoreTake(s_i2cMux, portMAX_DELAY);

    uint16_t snap[pins::NUM_GUNS];
    bool dirty = false;
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        snap[g] = s_shadow[g].load(std::memory_order_acquire);
        if (snap[g] != s_written[g]) dirty = true;
    }
    bool ok = true;
    if (dirty) {
        // fastWrite uses Vref=VDD, gain=1, normal mode for all channels in one I2C txn.
        ok = s_chip.fastWrite(snap[0], snap[1], snap[2], snap[3]);
        if (ok) {
            for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_written[g] = snap[g];
        } else {
            evt::postError("dac", "i2c_write_failed");
        }
    }

    if (s_i2cMux) xSemaphoreGive(s_i2cMux);
    return ok;
}

bool blockingSetCode(uint8_t g, uint16_t code) {
    if (g >= pins::NUM_GUNS) return false;
    setTarget(g, code);
    writeAllChannels();
    return isApplied(g, code);
}

static void dacTask(void*) {
    for (;;) {
        // Wait for a notification, then drain one batch.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        writeAllChannels();
    }
}

bool init() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_shadow[g].store(0);

    if (!s_i2cMux) s_i2cMux = xSemaphoreCreateMutex();

    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(400000);

    if (!s_chip.begin(0x60)) {           // default MCP4728 I2C address
        evt::postError("dac", "init_failed");
        return false;
    }
    // Configure all 4 channels: Vref = VDD, gain = 1x, output enabled, code = 0.
    for (uint8_t ch = 0; ch < 4; ++ch) {
        s_chip.setChannelValue((MCP4728_channel_t)ch, 0,
                               MCP4728_VREF_VDD,
                               MCP4728_GAIN_1X,
                               MCP4728_PD_MODE_NORMAL,
                               false /*udac*/);
        s_written[ch] = 0;
    }
    s_chipReady.store(true, std::memory_order_release);

    xTaskCreatePinnedToCore(dacTask, "dac", 4096, nullptr, 6, &s_task, 1);
    return true;
}

void blockingSafeAll() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) setTarget(g, SAFE_CODE);
    writeAllChannels();
}

} // namespace dac
