// main/game_darkroom/dr_config.h —— 《小黑屋》全局常量:资源表、容量、节奏。
// 纯数据与常量,不依赖 ESP-IDF/LVGL,主机可测。
// 数值口径:对齐原版 A Dark Room(doublespeakgames/adarkroom,2026-09-29 源码取证),
// 详见 DESIGN.zh_CN.md §9「原版机制全量分析」。本作不新增原版没有的玩法。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 存档 ----
#define DR_SAVE_VERSION   3u      // 存档结构版本,迁移见 dr_state.h
#define DR_SAVE_NVS_KEY   "dr_save"
#define DR_FLAG_BITS      64      // 剧情标记位数(事件表引用 0..63)

// ---- 资源(原版 stores;布局红线:只在尾部追加,不得中间插入) ----
typedef enum {
    DR_RES_WOOD = 0,      // 木材
    DR_RES_FUR,           // 毛皮
    DR_RES_MEAT,          // 肉
    DR_RES_BAIT,          // 诱饵
    DR_RES_WATER,         // 水(远征消耗)
    DR_RES_FOOD,          // 干肉(原版 cured meat;熏肉匠产出,M4 远征口粮)
    DR_RES_IRON,          // 铁
    DR_RES_COAL,          // 煤
    DR_RES_STEEL,         // 钢
    DR_RES_SULPHUR,       // 硫磺
    DR_RES_BULLETS,       // 子弹
    DR_RES_ALIEN,         // 异星金属
    DR_RES_CHARM,         // 护符
    DR_RES_LEATHER,       // 皮革(v2 尾插)
    DR_RES_SCALES,        // 鳞(v3 尾插;陷阱掉落/贸易)
    DR_RES_TEETH,         // 牙(v3 尾插;陷阱掉落/贸易)
    DR_RES_CLOTH,         // 布(v3 尾插;陷阱掉落)
    DR_RES_KIND_COUNT,
} dr_res_t;

// ---- 建筑(全量槽位;名称与造价在 dr_rules.c,对齐原版 Craftables) ----
#define DR_BUILDING_KINDS 40

// ---- 时间与经济(对齐原版;收入引擎见 dr_rules.c income_step) ----
#define DR_TICK_MS         1000u   // 全局心跳
#define DR_ECONOMY_TICK_S  10u     // 原版收入结算周期(workers income delay)
#define DR_OFFLINE_CAP_S   (8u * 3600u)   // 设备适配:离线补算上限 8h
// 超出上限部分按此折算(千分比):25% → 250(设备适配,原版无离线)
#define DR_OFFLINE_OVER_PERMILLE  250u

// ---- 事件系统 ----
#define DR_EVENT_MAX_CONDS 4      // 每个事件最多触发条件(与)
#define DR_EVENT_MAX_CHOICES 4    // 每个事件最多选项
#define DR_EVENT_COOLDOWN_MAX_S  (30u * 86400u)  // 冷却上限 30 天,防溢出

#ifdef __cplusplus
}
#endif
