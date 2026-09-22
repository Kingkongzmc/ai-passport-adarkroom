#pragma once
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 4
static inline uint32_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 0x100000;  // 主机模拟: pretend 1MB
}
