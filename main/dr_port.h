// main/dr_port.h —— 存档的设备适配层:NVS 读写 + Unix 时间戳。
// 引擎层(dr_state)只认镜像字节;本层负责 nvs_flash 与时间源,
// 并提供离线结算入口(读档 → Δt → 交给引擎)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 nvs_flash(已初始化则跳过)。返回 esp 错误码转译:0=成功。
int dr_port_storage_init(void);

// 当前 Unix 秒(esp_rtc/gettimeofday;deep sleep 期间由 RTC 维持,
// 电量耗尽 RTC 丢失时会发生回拨,由 dr_offline_ticks 的兜底逻辑处理)。
uint32_t dr_port_now_ts(void);

// 存档 → NVS。返回 0=成功。
int dr_port_save(const dr_game_t *g);

// NVS → 档。*out_loaded=false 且返回 0 表示空档(调用方自行 init 新档)。
// CRC/版本失败同样返回空档并打日志(坏档重开,不阻塞游戏)。
// *out_offline_ticks 返回本次离线折算 tick 数(空档为 0)。
int dr_port_load(dr_game_t *g, bool *out_loaded, uint32_t *out_offline_ticks);

#ifdef __cplusplus
}
#endif
