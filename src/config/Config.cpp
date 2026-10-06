#include "Config.h"

namespace cfg {

RuntimeConfig             Config::buf_[2];
std::atomic<uint8_t>      Config::activeIdx_{0};

SystemState g_sys;

void Config::init() {
    // Defaults already set via in-class initializers; ensure scratch == active.
    buf_[1] = buf_[0];
    activeIdx_.store(0, std::memory_order_release);
}

// IRAM: called from GPIO / PCNT ISRs, which keep running while Core 0 writes
// flash (cache off).  buf_ / activeIdx_ are internal DRAM.
const RuntimeConfig* IRAM_ATTR Config::active() {
    return &buf_[activeIdx_.load(std::memory_order_acquire)];
}

RuntimeConfig* Config::editScratch() {
    // Scratch = the buffer that is NOT currently active.
    uint8_t inactive = activeIdx_.load(std::memory_order_acquire) ^ 1u;
    // Seed scratch with the latest active contents so partial edits inherit defaults.
    buf_[inactive] = buf_[inactive ^ 1u];
    return &buf_[inactive];
}

void Config::publish() {
    uint8_t inactive = activeIdx_.load(std::memory_order_acquire) ^ 1u;
    activeIdx_.store(inactive, std::memory_order_release);
}

} // namespace cfg
