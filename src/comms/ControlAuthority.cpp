#include "ControlAuthority.h"
#include "config/Config.h"

#include <Arduino.h>
#include <atomic>

namespace control {

static constexpr uint32_t WEB_LEASE_MS = 3000;
static std::atomic<Owner> s_owner{Owner::Uart};
static std::atomic<uint32_t> s_webLeaseUntilMs{0};

static void expireLease() {
    if (s_owner.load(std::memory_order_acquire) != Owner::Web) return;
    uint32_t now = millis();
    uint32_t until = s_webLeaseUntilMs.load(std::memory_order_acquire);
    if ((int32_t)(now - until) >= 0 &&
        !cfg::g_sys.active.load(std::memory_order_acquire)) {
        s_owner.store(Owner::Uart, std::memory_order_release);
    }
}

Owner owner() {
    expireLease();
    return s_owner.load(std::memory_order_acquire);
}

bool canUse(Owner candidate) {
    return owner() == candidate;
}

bool acquireWeb() {
    expireLease();
    if (cfg::g_sys.active.load(std::memory_order_acquire)) return false;
    s_webLeaseUntilMs.store(millis() + WEB_LEASE_MS, std::memory_order_release);
    s_owner.store(Owner::Web, std::memory_order_release);
    return true;
}

bool heartbeatWeb() {
    if (owner() != Owner::Web) return false;
    uint32_t now = millis();
    s_webLeaseUntilMs.store(now + WEB_LEASE_MS, std::memory_order_release);
    cfg::g_sys.lastCmdMs.store(now, std::memory_order_release);
    return true;
}

void releaseWeb() {
    if (s_owner.load(std::memory_order_acquire) == Owner::Web) {
        s_owner.store(Owner::Uart, std::memory_order_release);
    }
}

const char* ownerName() {
    return owner() == Owner::Web ? "web" : "uart";
}

} // namespace control
