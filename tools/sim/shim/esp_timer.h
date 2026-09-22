#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);

// 模拟器垫片:tbeat 诊断定时器用不上,给空实现
typedef void *esp_timer_handle_t;
typedef struct { const char *name; void (*callback)(void *); } esp_timer_create_args_t;
static inline int esp_timer_create(const esp_timer_create_args_t *a,
                                   esp_timer_handle_t *h) {
    (void)a; *h = (void *)1; return 0;
}
static inline int esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) {
    (void)h; (void)us; return 0;
}
static inline int esp_timer_stop(esp_timer_handle_t h) { (void)h; return 0; }
static inline int esp_timer_delete(esp_timer_handle_t h) { (void)h; return 0; }
