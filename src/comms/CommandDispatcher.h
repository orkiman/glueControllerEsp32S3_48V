#pragma once
#include <stddef.h>

namespace cmd {

struct Result {
    bool ok;
    char cmd[20];
    char reason[24];
};

Result dispatch(const char* line, size_t len);

} // namespace cmd
