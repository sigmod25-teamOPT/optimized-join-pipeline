#pragma once

#include <string>
#include <cstring>
#include <cassert>
#include <utils.h>

// IMPORTANT This must be power of 2
struct alignas(8) ColString {
    uint32_t pageid;
    uint32_t offset;

    inline bool is_long_string(void) const {
        return offset == -1u;
    }
};
