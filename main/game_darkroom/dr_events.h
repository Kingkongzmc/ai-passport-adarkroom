// main/game_darkroom/dr_events.h —— 数据驱动事件系统:条件/效果操作码 + 事件表运行时。
// 对应原作 JS 事件框架的 C 重实现,见 DESIGN.zh_CN.md §4.1 与 §9.9。
// 纯 C99,主机可测;事件文案在 dr_text.h,事件表数据在 dr_events_data.c(M6 灌装)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr_config.h"
#include "dr_state.h"
#include "dr_text.h"
#include "dr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 触发场景(位掩码,事件可挂多个场景) ----
enum {
    DR_SCENE_HOME    = 1u << 0,  // 主页(小屋,原版 Room 模块)
    DR_SCENE_VILLAGE = 1u << 1,  // 村庄页(原版 Outside 模块)
    DR_SCENE_MAP     = 1u << 2,  // 荒野地图
    DR_SCENE_ROOM    = 1u << 3,  // 地点内部房间
    DR_SCENE_COMBAT  = 1u << 4,  // 战斗
    DR_SCENE_ANY     = 0xFFu,
};

// ---- 条件操作码(全为"条件为真"语义,多条之间与) ----
typedef enum {
    DR_COND_RES_GE = 0,   // 资源量 >= arg2
    DR_COND_RES_LT,       // 资源量 <  arg2
    DR_COND_BUILDING_GE,  // 建筑(arg1)等级 >= arg2
    DR_COND_FLAG_SET,     // 剧情标记(arg1, 0..63)已置位
    DR_COND_FLAG_CLEAR,   // 剧情标记未置位
    DR_COND_EVENT_SEEN,   // 事件(arg1)触发过至少一次
    DR_COND_CHANCE,       // 千分比概率(arg2)命中(用运行 RNG)
    DR_COND_POP_GE,       // 人口 >= arg2
    DR_COND_BUILDING_LT,  // 建筑(arg1)等级 < arg2(M6:阴险工匠 hut<20 等门)
    DR_COND_POP_LT,       // 人口 < arg2(M6:疫病 10<pop<50 等门)
    DR_COND_NONE = 0xFF,  // 条目占位:恒真(填未尽槽位)
} dr_cond_op_t;

typedef struct {
    uint8_t  op;        // dr_cond_op_t
    uint8_t  arg1;      // 资源 id / 建筑 id / 标记位 / 事件 id(按 op 解释)
    uint16_t arg2;      // 阈值 / 千分比
} dr_cond_t;

// ---- 效果操作码(选项被选中时依序执行;跳转类先到先得) ----
typedef enum {
    DR_EFF_RES_ADD = 0,  // 资源(arg1) += arg2(可为负值,存量钳到 0)
    DR_EFF_FLAG_SET,     // 置位标记 arg1
    DR_EFF_FLAG_CLEAR,   // 清除标记 arg1
    DR_EFF_GOTO_EVENT,   // 立即触发事件(arg1)——对话框链
    DR_EFF_END_ACT,      // 幕推进标记(arg1):UI 据此解锁页面(数值留在 flags)
    DR_EFF_POP_ADD,      // 人口 += arg2(可为负,钳到 0;减员同步裁岗)
    DR_EFF_BLD_ADD,      // 建筑(arg1)等级 += arg2(钳到 ≥0;M6:失火毁屋/白盖屋)
    DR_EFF_BLD_FRAC,     // 建筑(arg1)损失 floor(等级×arg2/1000),至少 1(M6:毁陷阱)
    DR_EFF_POP_FRAC,     // 人口损失 floor(人口×arg2/1000),至少 1,同步裁岗(M6:疫病)
    DR_EFF_GOTO_W,       // 以千分比(arg2)概率跳到事件 arg1;未中则继续后续效果
                          // (M6:原版加权 nextScene 分支的移植形态,先到先得)
    DR_EFF_THIEF_HANG,   // 吊死窃贼:停止偷窃,追回全部被偷物资(原版 global.js)
    DR_EFF_THIEF_SPARE,  // 放过窃贼:停止偷窃,得 stealthy Perk(原版)
    DR_EFF_NONE = 0xFF,
} dr_eff_op_t;

typedef struct {
    uint8_t  op;         // dr_eff_op_t
    uint8_t  arg1;
    int16_t  arg2;       // DR_EFF_RES_ADD/POP_ADD/BLD_ADD 允许负数
} dr_eff_t;

// ---- 事件与选项 ----
// cost_res 槽位特值:0xFF=无代价;0xFE=旗标代价——此时 cost_amt 存旗标位,
// 要求已持有(置位)才可选,选中即消耗(清除)。宗师的火把 1 顶(原版
// cost: {'torch': 1};火把是消耗性旗标,不在资源表)。
#define DR_COST_NONE  0xFFu
#define DR_COST_FLAG  0xFEu

typedef struct {
    uint16_t text_id;                 // 选项文案 id(dr_text.h)
    uint8_t  avail_flag_clear;        // 需未置位的标记(0xFF=恒可见;已有则灰)
    uint8_t  cost_res[3];             // 代价资源(M6 扩容 2→3:游牧商人罗盘)
    uint16_t cost_amt[3];             // 代价数量(与 cost_res 一一对应)
    dr_eff_t effects[4];              // 依序执行,遇到跳转已定则跳过后续跳转
} dr_event_choice_t;

typedef struct {
    uint16_t text_id;                 // 标题/正文文案 id(dr_text.h)
    uint8_t  scene_mask;              // DR_SCENE_*(触发场景;0=仅链式可达,不轮询)
    uint8_t  priority;                // 同场景多事件命中时高者优先
    uint16_t max_seen;                // 最多触发次数(1=一次性剧情;0=不限)
    uint32_t cooldown_s;              // 触发后冷却秒数(离线照算)
    dr_cond_t conds[DR_EVENT_MAX_CONDS];
    dr_event_choice_t choices[DR_EVENT_MAX_CHOICES];
    uint8_t choice_count;
} dr_event_t;

// ---- 运行时 ----

// 事件判定结果。
typedef enum {
    DR_EVENT_NONE = 0,     // 无事件触发
    DR_EVENT_FIRED,        // 触发,等待玩家选择
} dr_event_result_t;

// 触发上下文:RNG 用游戏档内的确定性序列;now_ts 用于冷却判定。
// events 为事件表,count 为条数(由 dr_events_data.c 提供 dr_events_table())。
const dr_event_t *dr_events_table(uint16_t *count);

// 会话内冷却与全局节拍由调用方持有的会话结构记账,主机可测。
// next_ok_ts:全局节拍门——任何事件触发后,最早下一次触发时刻
// (原版 Events._EVENT_TIME_RANGE 3~6 分钟;由 pick 在触发时掷定)。
typedef struct {
    uint32_t last_fire_ts[128];       // 事件 id → 本会话上次触发时间戳
    uint32_t next_ok_ts;              // 全局节拍门(0=不设门)
} dr_event_session_t;

void dr_event_session_init(dr_event_session_t *s);

// 返回触发的下标(*out_idx),或 DR_EVENT_NONE;同场景命中多个取 priority 最高、
// id 最小者(确定性,便于测试)。rng 使用 g 内种子派生的独立序列(不污染地图种子)。
// 触发时顺带掷定下一次的全局节拍门(180+rand(180) 秒,原版 3~6 分钟)。
dr_event_result_t dr_event_pick(const dr_event_t *events, uint16_t count,
                                const dr_game_t *g, dr_event_session_t *sess,
                                uint32_t now_ts, uint8_t scene,
                                dr_rng_t *rng, uint16_t *out_idx);

// 轮询封装(UI 心跳入口):以 g->rng_seed_state 派生独立序列挑选事件,
// **无论是否触发都把推进后的序列写回 g**——CHANCE 未命中若不回写,
// 下次轮询重新播种会永远重掷同一个值(§3.3 硬化)。
dr_event_result_t dr_event_poll(const dr_event_t *events, uint16_t count,
                                dr_game_t *g, dr_event_session_t *sess,
                                uint32_t now_ts, uint8_t scene,
                                uint16_t *out_idx);

// 选项代价是否付得起(UI 灰显/跳过焦点用;choose 内还会再核一次)。
bool dr_event_choice_affordable(const dr_event_t *e, uint8_t choice_idx,
                                const dr_game_t *g);

// 执行事件的某个选项(条件在 pick 时已判过)。应用效果并记账:
// 代价扣除、event_count++、last_fire_ts 更新。
// 代价不足返回 -2 且不产生任何效果;返回 DR_EFF_GOTO_EVENT 链到的下一事件
// 下标(无链返回 -1)。rng 供 DR_EFF_GOTO_W 掷点(会话内持久流,调用方持有)。
#define DR_EVENT_CHOOSE_REJECTED (-2)
int16_t dr_event_choose(const dr_event_t *events, uint16_t count,
                        dr_game_t *g, dr_event_session_t *sess,
                        uint32_t now_ts, dr_rng_t *rng,
                        uint16_t event_idx, uint8_t choice_idx);

#ifdef __cplusplus
}
#endif
