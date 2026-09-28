// main/game_darkroom/dr_config.h —— 《小黑屋》全局常量:资源表、容量、节奏。
// 纯数据与常量,不依赖 ESP-IDF/LVGL,主机可测。
// 数值口径见 docs/GAMEPLAY.zh_CN.md §6;具体数值表在 M6 分批灌装。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 存档 ----
#define DR_SAVE_VERSION   2u      // 存档结构版本,迁移见 dr_state.h
#define DR_SAVE_NVS_KEY   "dr_save"
#define DR_FLAG_BITS      64      // 剧情标记位数(事件表引用 0..63)

// ---- 资源 ----
// M1 引擎层先定义全量资源槽位;各资源的解锁与产出规则在数值表(M3+)灌装。
// 布局红线:只能在尾部追加,不得在中间插入——res[]/res_total[] 按枚举下标
// 落盘,插入会改写历史存档的字段含义(v2 追加皮革即遵守此规)。
typedef enum {
    DR_RES_WOOD = 0,      // 木材
    DR_RES_FUR,           // 毛皮
    DR_RES_MEAT,          // 肉
    DR_RES_BAIT,          // 诱饵
    DR_RES_WATER,         // 水(远征消耗)
    DR_RES_FOOD,          // 食物(村民与远征消耗)
    DR_RES_IRON,          // 铁
    DR_RES_COAL,          // 煤
    DR_RES_STEEL,         // 钢
    DR_RES_SULPHUR,       // 硫磺
    DR_RES_BULLETS,       // 子弹
    DR_RES_ALIEN,         // 异星金属
    DR_RES_CHARM,         // 护符
    DR_RES_LEATHER,       // 皮革(v2 尾插;制革匠产出,皮甲原料)
    DR_RES_KIND_COUNT,
} dr_res_t;

// ---- 村庄口粮(M3) ----
// 每人每经济 tick 吃 1 口粮:先扣食物(DR_RES_FOOD),不足再扣肉;
// 两皆空 → 全村罢工(职业停工,闲人仍拾荒求生),不死亡。口径见 GAMEPLAY §3。
#define DR_FOOD_PER_VILLAGER  1u

// ---- 皮甲(M3;战斗减免 M4 接入) ----
#define DR_TRADE_ARMOR_WOOD     50u
#define DR_TRADE_ARMOR_LEATHER  10u

// ---- 建筑(全量槽位;名称与造价在 M2 起的数值表里) ----
#define DR_BUILDING_KINDS  40

// ---- 时间与经济 ----
#define DR_TICK_MS         1000u   // 全局心跳
#define DR_ECONOMY_TICK_S  10u     // 一个经济 tick(产出/消耗按此结算)
#define DR_OFFLINE_CAP_S   (8u * 3600u)   // 离线全速结算上限 8h
// 超出上限部分按此折算(千分比):25% → 250
#define DR_OFFLINE_OVER_PERMILLE  250u

// ---- 事件系统 ----
#define DR_EVENT_MAX_CONDS  4      // 每个事件最多触发条件(与)
#define DR_EVENT_MAX_CHOICES 4     // 每个事件最多选项
#define DR_EVENT_COOLDOWN_MAX_S  (30u * 86400u)  // 冷却上限 30 天,防溢出

#ifdef __cplusplus
}
#endif
