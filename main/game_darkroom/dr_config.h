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
#define DR_SAVE_VERSION   6u      // 存档结构版本,迁移见 dr_state.h(v6:能量元件/星舰/周目)
#define DR_SAVE_NVS_KEY   "dr_save"
#define DR_FLAG_BITS      64      // 剧情标记位数(事件表引用 0..63)
// 存档码功能开关(M7):导入要逐字符输 200+ 字符,体验未到位先隐藏;
// 串口导入就绪后置 1 重新放出(代码全保留,仅设置页行与入口受此门)。
#define DR_FEATURE_SAVECODE 0

// 剧情标记位(flags;规则层与事件表共用;M4:到访矿并回家后置位)
#define DR_FLAG_IRON_MINE     4u
#define DR_FLAG_COAL_MINE     5u
#define DR_FLAG_SULPHUR_MINE  6u
// 工坊制造品(一次性,§9.7 原版 crafts)
#define DR_FLAG_WATERSKIN     7u   // 水袋:水 +10
#define DR_FLAG_CASK          8u   // 木桶:水 +20
#define DR_FLAG_TANK          9u   // 水箱:水 +50
#define DR_FLAG_RUCKSACK     10u   // 背囊:背袋 +10
#define DR_FLAG_WAGON        11u   // 篷车:背袋 +30
#define DR_FLAG_CONVOY       12u   // 车队:背袋 +60
#define DR_FLAG_TORCH        13u   // 火把:探索洞穴用(1木+1布,消耗品)
// M6 切片二(空位 14 起;Perk 以旗标持久,原版 $SM.hasPerk 同义)
#define DR_FLAG_CITY_CLEARED    14u  // 城市地点搜剿胜利(原版 game.cityCleared)
#define DR_FLAG_PERK_SCOUT      15u  // 侦查:揭示半径翻倍+开机确定性揭示一片
#define DR_FLAG_PERK_EVASIVE    16u  // 闪避:敌方命中 ×0.8(原版 events.js)
#define DR_FLAG_PERK_PRECISE    17u  // 精准:玩家命中 +10%(原版 getHitChance)
#define DR_FLAG_PERK_BARBARIAN  18u  // 蛮力:近战伤害 ×1.5 向下取整(原版)
#define DR_FLAG_PERK_STEALTHY   19u  // 潜行:遭遇触发率减半(原版 FIGHT_CHANCE)
#define DR_FLAG_THIEF_ACTIVE    20u  // 窃贼进行中(解决时清除;事件轮询门)
// M5 第四幕(星舰/结局;空位 21 起)
#define DR_FLAG_SHIP_OPEN       21u  // 星舰页签解锁(踏上坠毁星舰格,原版 features.location.spaceShip)
#define DR_FLAG_SHIP_SEEN       22u  // 首访星舰页通知已发(原版 spaceShip.seenShip)
#define DR_FLAG_LIFT_WARNED     23u  // 升空确认"不再回来"已见(原版 spaceShip.seenWarning)
// M7 地标回补(§4.1.1;空位 24 起——20-23 已被窃贼/星舰占用,
// DESIGN §9.8 补证段预估的 20-22 让位)
#define DR_FLAG_PERK_GASTRONOME 24u  // 美食家:吃干肉回血 8→16(沼泽谈话,原版 gastronome)
#define DR_FLAG_SWAMP_DONE      25u  // 沼泽谈话已完成(一次性,原版 markVisited)
#define DR_FLAG_RUIN_TAKEN      26u  // 被毁村庄补给已拿(一次性;本作简化 collectStores)

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
    DR_RES_MEDICINE,      // 药(v4 尾插;贸易购入,远征治疗 +20HP)
    DR_RES_ENERGY_CELL,   // 能量元件(v6 尾插;病人事件/激光步枪弹药,M5)
    DR_RES_GRENADE,       // 手雷(v6 尾插;制造台造,战斗消耗品 15 伤)
    DR_RES_BOLAS,         // 流星锤(v6 尾插;制造台造,战斗消耗品 眩晕)
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
#define DR_EVENT_MAX_CHOICES 5    // 每个事件最多选项(M6 切片二:游牧商人 5)
#define DR_EVENT_COOLDOWN_MAX_S  (30u * 86400u)  // 冷却上限 30 天,防溢出

#ifdef __cplusplus
}
#endif
