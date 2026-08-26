#pragma once
#include <stdint.h>

namespace control {

enum class Owner : uint8_t { Uart = 0, Web = 1 };

Owner owner();
bool canUse(Owner candidate);
bool acquireWeb();
bool heartbeatWeb();
void releaseWeb();
const char* ownerName();

} // namespace control
