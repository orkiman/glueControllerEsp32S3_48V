#include "Dac.h"
#include "comms/Events.h"

#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace dac {

static constexpr uint8_t  I2C_ADDR       = 0x60;   // MCP4728 default address
static constexpr float    VDD            = 3.3f;
static constexpr uint16_t DAC_FULL_SCALE = 4095;
static constexpr uint8_t  WRITE_ATTEMPTS = 3;      // immediate retries per request
static constexpr uint32_t ERROR_EVENT_MS = 1000;   // at most one i2c error event per second

static TaskHandle_t      s_task      = nullptr;
static std::atomic<bool> s_chipReady{false};
// Serializes chip access between dacTask and a synchronous blockingSetCode()
// (fire() on the RT core).
static SemaphoreHandle_t s_i2cMux    = nullptr;
static uint32_t          s_lastErrorMs = 0;
static bool              s_errorSent   = false;

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

// ---------------- MCP4728 commands ----------------
// Every channel uses VREF = VDD, gain 1x, normal power (all those bits 0).
static bool i2cWrite(const uint8_t* buf, size_t n) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(buf, n);
    return Wire.endTransmission() == 0;
}

// Multi-Write: 3 bytes per channel (0 1 0 0 0 DAC1 DAC0 UDAC, then the
// code).  Used for one or two channels: 4 or 7 bytes on the bus.
static bool multiWrite(const uint16_t* codes, uint8_t mask) {
    uint8_t buf[3 * pins::NUM_GUNS];
    size_t  n = 0;
    for (uint8_t ch = 0; ch < pins::NUM_GUNS; ++ch) {
        if (!(mask & (1u << ch))) continue;
        buf[n++] = (uint8_t)(0x40 | (ch << 1));          // UDAC = 0: output updates now
        buf[n++] = (uint8_t)((codes[ch] >> 8) & 0x0F);
        buf[n++] = (uint8_t)(codes[ch] & 0xFF);
    }
    return i2cWrite(buf, n);
}

// Fast Write: all four channels, 2 bytes each (8 bytes on the bus).
static bool fastWrite(const uint16_t* codes) {
    uint8_t buf[2 * pins::NUM_GUNS];
    for (uint8_t ch = 0; ch < pins::NUM_GUNS; ++ch) {
        buf[2 * ch]     = (uint8_t)((codes[ch] >> 8) & 0x0F);  // 0 0 PD1 PD0 D11..D8
        buf[2 * ch + 1] = (uint8_t)(codes[ch] & 0xFF);
    }
    return i2cWrite(buf, sizeof(buf));
}

static void reportWriteError() {
    uint32_t now = millis();
    if (s_errorSent && now - s_lastErrorMs < ERROR_EVENT_MS) return;
    s_errorSent   = true;
    s_lastErrorMs = now;
    evt::postError("dac", "i2c_write_failed");
}

// Write every channel whose target differs from the chip.  Task context only.
static bool writePending() {
    if (!s_chipReady.load(std::memory_order_acquire)) return false;

    if (s_i2cMux) xSemaphoreTake(s_i2cMux, portMAX_DELAY);

    uint16_t snap[pins::NUM_GUNS];
    uint8_t  mask  = 0;
    uint8_t  count = 0;
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        snap[g] = s_shadow[g].load(std::memory_order_acquire);
        if (snap[g] != s_written[g]) { mask |= (uint8_t)(1u << g); count++; }
    }
    bool ok = true;
    if (count) {
        ok = (count >= 3) ? fastWrite(snap) : multiWrite(snap, mask);
        if (ok) {
            for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
                if (count >= 3 || (mask & (1u << g))) s_written[g] = snap[g];
            }
        } else {
            reportWriteError();
        }
    }

    if (s_i2cMux) xSemaphoreGive(s_i2cMux);
    return ok;
}

static bool anyPending() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) {
        if (s_shadow[g].load(std::memory_order_acquire) != s_written[g]) return true;
    }
    return false;
}

bool blockingSetCode(uint8_t g, uint16_t code) {
    if (g >= pins::NUM_GUNS) return false;
    setTarget(g, code);
    for (uint8_t i = 0; i < WRITE_ATTEMPTS && !isApplied(g, code); ++i) writePending();
    return isApplied(g, code);
}

static void dacTask(void*) {
    for (;;) {
        // Wait for a request; while a write keeps failing, retry every tick.
        ulTaskNotifyTake(pdTRUE, anyPending() ? 1 : portMAX_DELAY);
        for (uint8_t i = 0; i < WRITE_ATTEMPTS; ++i) {
            if (writePending()) break;
        }
    }
}

// ---------------- power-on value (EEPROM) ----------------
// At power-up the MCP4728 loads its EEPROM (factory default 0 V).  A 0 V
// threshold leaves the comparator undecided at zero current, and the MUX
// select pins float until the firmware starts, so SAFE_CODE is stored once:
// every comparator then reads "no current" and keeps IN2 low.
static bool readChip(uint8_t* r) {
    // 24 bytes: per channel 3 bytes DAC register + 3 bytes EEPROM.
    if (Wire.requestFrom((uint8_t)I2C_ADDR, (uint8_t)24) != 24) return false;
    for (uint8_t i = 0; i < 24; ++i) r[i] = (uint8_t)Wire.read();
    return true;
}

static bool eepromIsSafe(const uint8_t* r) {
    for (uint8_t ch = 0; ch < pins::NUM_GUNS; ++ch) {
        uint8_t  hi   = r[6 * ch + 4];
        uint8_t  lo   = r[6 * ch + 5];
        uint16_t code = (uint16_t)(((hi & 0x0F) << 8) | lo);
        if ((hi & 0xF0) != 0 || code != SAFE_CODE) return false;   // VREF, PD, gain must be 0
    }
    return true;
}

static void ensureSafeEeprom() {
    uint8_t r[24];
    if (!readChip(r)) { evt::postError("dac", "eeprom_read_failed"); return; }
    if (eepromIsSafe(r)) return;

    // Sequential Write from channel A: writes the DAC registers and EEPROM.
    uint8_t w[1 + 2 * pins::NUM_GUNS];
    w[0] = 0x50;                                         // 0 1 0 1 0 DAC1=0 DAC0=0 UDAC=0
    for (uint8_t ch = 0; ch < pins::NUM_GUNS; ++ch) {
        w[1 + 2 * ch] = (uint8_t)((SAFE_CODE >> 8) & 0x0F);
        w[2 + 2 * ch] = (uint8_t)(SAFE_CODE & 0xFF);
    }
    if (!i2cWrite(w, sizeof(w))) { evt::postError("dac", "eeprom_write_failed"); return; }
    vTaskDelay(pdMS_TO_TICKS(60));                       // EEPROM write: 50 ms max

    if (!readChip(r) || !eepromIsSafe(r)) evt::postError("dac", "eeprom_verify_failed");
}

bool init() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_shadow[g].store(SAFE_CODE);

    if (!s_i2cMux) s_i2cMux = xSemaphoreCreateMutex();

    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(400000);

    Wire.beginTransmission(I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        evt::postError("dac", "init_failed");
        return false;
    }
    ensureSafeEeprom();

    // Known state on every channel: VREF = VDD, gain 1x, normal, SAFE_CODE.
    uint16_t codes[pins::NUM_GUNS];
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) codes[g] = SAFE_CODE;
    if (!multiWrite(codes, 0x0F)) {
        evt::postError("dac", "init_failed");
        return false;
    }
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) s_written[g] = SAFE_CODE;
    s_chipReady.store(true, std::memory_order_release);

    xTaskCreatePinnedToCore(dacTask, "dac", 4096, nullptr, 6, &s_task, 1);
    return true;
}

void blockingSafeAll() {
    for (uint8_t g = 0; g < pins::NUM_GUNS; ++g) setTarget(g, SAFE_CODE);
    for (uint8_t i = 0; i < WRITE_ATTEMPTS && anyPending(); ++i) writePending();
}

} // namespace dac
