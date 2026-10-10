#pragma once
// 模拟器垫片:电源管理 API 空转。
// 浅睡打盹在主机上是纯空转(sleep_req 会被 tick 重新评估后保持,不会死循环:
// 虚拟时间只由主循环推进,key_task 快速空转让出 CPU);深睡真机不返回,
// 垫片同样挂死该线程(主线程继续,进程正常退出)。
#include <stdint.h>

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_GPIO,
} esp_sleep_wakeup_cause_t;

typedef enum {
    ESP_GPIO_WAKEUP_GPIO_LOW = 0,
    ESP_GPIO_WAKEUP_GPIO_HIGH = 1,
} esp_sleep_gpio_wakeup_level;

static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) {
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}
static inline int esp_sleep_enable_timer_wakeup(uint64_t us) {
    (void)us;
    return 0;
}
static inline int esp_light_sleep_start(void) { return 0; }
static inline void esp_deep_sleep_enable_gpio_wakeup(
    uint64_t gpio_pin_mask, esp_sleep_gpio_wakeup_level level) {
    (void)gpio_pin_mask;
    (void)level;
}
static inline void esp_deep_sleep_start(void) {
    for (;;) { /* 真机不返回;模拟器挂死调用线程,主线程照常退出 */ }
}
