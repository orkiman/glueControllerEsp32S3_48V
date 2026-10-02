#pragma once
#include <stddef.h>
#include "LiveSync.h"

namespace cmd {

struct Result {
    bool ok;
    char cmd[20];
    char reason[24];
};

// Parse and execute one JSON command.  Takes the livesync edit lock for the
// whole command, so the UART and web callers may run concurrently.  `src`
// tells livesync who issued it, so the change is pushed to the other side.
Result dispatch(const char* line, size_t len, livesync::Source src);

} // namespace cmd
