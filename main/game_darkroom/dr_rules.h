// main/game_darkroom/dr_rules.h —— 第一幕"小黑屋"经济规则:火焰、伐木、陷阱、建造。
// 纯 C99,主机可测;数值口径 docs/GAMEPLAY.zh_CN.md §2。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr_state.h"
#include "dr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 建筑(id 与 dr_game_t.building_lv 下标一致) ----
typedef enum {
    DR_BLD_CART = 0,    // 板车:陷阱解锁前置
    DR_BLD_TRAP,        // 陷阱:周期带回毛皮/肉/诱饵
    DR_BLD_HUT,         // 小屋:每级 +2 人口上限
    DR_BLD_LODGE,       // 猎人小屋:允许村民当猎人(M3)
    DR_BLD_TRADE_POST,  // 贸易站(M3)
    DR_BLD_KIND_COUNT,
} dr_building_t;

// ---- 火焰(5 档,对齐原版 FireEnum) ----
typedef enum {
    DR_FIRE_DEAD = 0,       // 熄灭
    DR_FIRE_SMOLDERING,     // 微弱
    DR_FIRE_FLICKERING,     // 跳动
    DR_FIRE_BURNING,        // 旺盛
    DR_FIRE_ROARING,        // 炽烈
} dr_fire_t;

// 火焰衰减:每次点火/添柴后经此时长降一档(原版 _FIRE_COOL_DELAY 5 分钟)。
#define DR_FIRE_LEVEL_SECONDS  300u

// ---- 手动动作(除注明外对齐原版;节奏压缩见 docs/GAMEPLAY.zh_CN.md §2) ----
#define DR_GATHER_COOLDOWN_S   10u    // 收集木材冷却(设计压缩;原版 60s,有板车 +50 木)
#define DR_GATHER_WOOD         10     // 每次 +10 木
#define DR_STOKE_COOLDOWN_S    10u    // 点火/添柴共用冷却(原版 _STOKE_COOLDOWN)
#define DR_FIRE_LIGHT_COST     5      // 点火(从熄灭):5 木,直接到"旺盛"
#define DR_FIRE_STOKE_COST     1      // 添柴(火燃着):1 木,+1 档封顶"炽烈"

// ---- 陷阱 ----
#define DR_TRAP_PERIOD_S       30u    // 每次结算周期
#define DR_TRAP_CHANCE_PERMILLE 500   // 50% 概率有收获

// ---- 建筑造价 ----
typedef struct {
    uint16_t wood, fur, meat;   // fur/meat = 额外条件(猎人小屋/贸易站)
} dr_bld_cost_t;
uint16_t dr_building_cost_wood(uint8_t building_id, uint8_t current_lv);
dr_bld_cost_t dr_building_cost(uint8_t building_id, uint8_t current_lv);

// ---- 查询 ----
dr_fire_t dr_rules_fire(const dr_game_t *g, uint32_t now_ts);
uint16_t  dr_rules_pop_cap(const dr_game_t *g);
bool      dr_rules_can_build(const dr_game_t *g, uint8_t building_id);
uint32_t  dr_rules_gather_ready_in(const dr_game_t *g, uint32_t now_ts);

// ---- 动作(返回 false = 条件不满足,未改变状态) ----
bool dr_rules_stoke_fire(dr_game_t *g, uint32_t now_ts);     // 生火/添柴
bool dr_rules_gather(dr_game_t *g, uint32_t now_ts);         // 收集木材
bool dr_rules_build(dr_game_t *g, uint8_t building_id, uint32_t now_ts);

// ---- 周期结算:由 UI 的 1s 心跳驱动;内部按 fire/gather/trap 的节律推进。
// now_ms 用毫秒(设备心跳),内部换算;返回 true 表示状态有变化(需要重绘)。
// 火焰计时基准不落盘 —— 每次存档后从 0 重新计,离线期火焰视为熄灭(符合剧情)。
typedef struct {
    uint32_t fire_deadline_ms;   // 当前火焰档到期时刻
    uint32_t gather_ready_ms;    // 伐木冷却到期时刻
    uint32_t stoke_ready_ms;     // 点火/添柴冷却到期时刻
    uint32_t trap_next_ms;       // 下次陷阱可收获时刻
    uint32_t econ_next_ms;       // 下次村庄产出结算时刻(10s 经济 tick)
    uint32_t wanderer_next_ms;   // 下次流浪者到达时刻(0.5~3min 随机)
    dr_rng_t rng;                // 陷阱概率(从 g->rng_seed_state 派生)
    bool     rng_inited;
} dr_rules_rt_t;

void dr_rules_rt_init(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms);
// 返回 true = 游戏数据有变化;若 *fire_out true 表示火焰档变化(重绘+可存档)。
bool dr_rules_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms, bool *fire_out);

// ---- 村庄职业(M3 提前实装:分配 + 每 10s 产出) ----
// 离线结算暂不含村庄产出(M2/M3 口径,灌装时接入 dr_offline_ticks)。
typedef enum {
    DR_JOB_LUMBER = 0,   // 伐木工:+2 木/tick·人(常开)
    DR_JOB_HUNTER,       // 猎人(需猎人小屋):+1 毛 +1 肉/tick·人
    DR_JOB_TANNER,       // 制革匠(M3+ 工作场所)
    DR_JOB_SMITH,        // 铁匠(M3+ 工作场所)
    DR_JOB_KIND_COUNT,
} dr_job_t;
// 未分配人口(采集者,原版 gatherer)自动 +1 木/tick·人。
uint16_t dr_rules_job_idle(const dr_game_t *g);
bool     dr_rules_job_unlocked(const dr_game_t *g, uint8_t job);
// delta 正=分配/负=撤下;失败(超员/无人可撤/职业锁定)返回 false。
bool     dr_rules_job_assign(dr_game_t *g, uint8_t job, int16_t delta);

// ---- 流浪者到达(M3:有房有火则随机 0.5~3 分钟来 1 人) ----
void     dr_rules_wanderer_schedule(dr_rules_rt_t *rt, uint32_t now_ms);
// 心跳内调用;到点且有空闲房源时 +1 人口,返回 true 并自动排下一次。
bool     dr_rules_wanderer_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms);

// ---- 离线结算(进应用时调用一次;复用 dr_offline_ticks 折算口径) ----
typedef struct {
    uint32_t ticks;          // 等效经济 tick 数
    uint32_t wood, fur, meat, bait;   // 离线所得(已入账)
    bool     fire_out;       // 离线导致火焰熄灭
} dr_offline_yield_t;
// delta = now_ts_s - g->saved_at_ts;火焰:离线即熄;村庄产出按等效 tick 补;
// 陷阱按离线周期数逐周期掷定(上限 DR_OFFLINE_CAP_S)。需在 rt_init 后调用。
uint32_t dr_rules_offline_settle(dr_rules_rt_t *rt, dr_game_t *g,
                                 uint32_t now_ts_s, uint32_t now_ms,
                                 dr_offline_yield_t *y);

// ---- 贸易(贸易站建成后可用;牌价:毛皮 5 木,肉 3 木,诱饵 3 木) ----
#define DR_TRADE_FUR_WOOD   5u
#define DR_TRADE_MEAT_WOOD  3u
#define DR_TRADE_BAIT_WOOD  3u
uint32_t dr_rules_trade_sell_value(const dr_game_t *g);
// 结算:清空毛皮/肉,木材入账(计入累计获得)。返回实际所得。
uint32_t dr_rules_trade_sell_all(dr_game_t *g);
bool     dr_rules_trade_fur10(dr_game_t *g);   // 10 毛皮 → 50 木
bool     dr_rules_trade_meat10(dr_game_t *g);  // 10 肉 → 30 木
bool     dr_rules_trade_bait5(dr_game_t *g);   // 15 木 → 5 诱饵

// ---- 陷阱:手动查看(同网页版) ----
// 倒计时走满后由玩家按"查看陷阱"结算:按陷阱等级掷收获,重置 30s 冷却。
// 返回收获位掩码(bit0 毛皮 bit1 肉 bit2 诱饵),0 = 这次落空。
#define DR_TRAP_GOT_FUR   0x1u
#define DR_TRAP_GOT_MEAT  0x2u
#define DR_TRAP_GOT_BAIT  0x4u
uint8_t dr_rules_trap_check(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms);
bool     dr_rules_trap_ready(const dr_rules_rt_t *rt, const dr_game_t *g,
                             uint32_t now_ms);

#ifdef __cplusplus
}
#endif
