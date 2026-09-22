// main/game_darkroom/dr_events.h —— 数据驱动事件系统:条件/效果操作码 + 事件表运行时。
// 对应原作 JS 事件框架的 C 重实现,见 DESIGN.zh_CN.md §4.1。
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
    DR_SCENE_HOME    = 1u << 0,  // 主页(小屋/村庄)
    DR_SCENE_VILLAGE = 1u << 1,  // 村庄页
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
    DR_COND_NONE = 0xFF,  // 条目占位:恒真(填未尽槽位)
} dr_cond_op_t;

typedef struct {
    uint8_t  op;        // dr_cond_op_t
    uint8_t  arg1;      // 资源 id / 建筑 id / 标记位 / 事件 id(按 op 解释)
    uint16_t arg2;      // 阈值 / 千分比
} dr_cond_t;

// ---- 效果操作码(选项被选中时依序执行) ----
typedef enum {
    DR_EFF_RES_ADD = 0,  // 资源(arg1) += arg2(可为负值,存量钳到 0)
    DR_EFF_FLAG_SET,     // 置位标记 arg1
    DR_EFF_FLAG_CLEAR,   // 清除标记 arg1
    DR_EFF_GOTO_EVENT,   // 立即触发事件(arg1)——对话框链
    DR_EFF_END_ACT,      // 幕推进标记(arg1):UI 据此解锁页面(数值留在 flags)
    DR_EFF_POP_ADD,      // 人口 += arg2(可为负,钳到 0)
    DR_EFF_NONE = 0xFF,
} dr_eff_op_t;

typedef struct {
    uint8_t  op;         // dr_eff_op_t
    uint8_t  arg1;
    int16_t  arg2;       // DR_EFF_RES_ADD 允许负数
} dr_eff_t;

// ---- 事件与选项 ----
typedef struct {
    uint16_t text_id;                 // 标题/正文文案 id(dr_text.h)
    uint8_t  scene_mask;              // DR_SCENE_*(触发场景)
    uint8_t  priority;                // 同场景多事件命中时高者优先
    uint16_t max_seen;                // 最多触发次数(1=一次性剧情;0=不限)
    uint32_t cooldown_s;              // 触发后冷却秒数(离线照算)
    dr_cond_t conds[DR_EVENT_MAX_CONDS];
    struct {
        uint16_t text_id;             // 选项文案 id
        dr_eff_t  effects[2];         // 每选项至多 2 个效果
    } choices[DR_EVENT_MAX_CHOICES];
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

// 在给定场景挑一个可触发事件:
//  1) 条件全真;2) 次数未超 max_seen;3) 冷却已过(冷却结束时间不落盘,
//     以"冷却起点 = 上次触发存不下来"→ 简化为:冷却以 event_count 与
//     saved_at_ts 的差不可恢复,故冷却只在会话内生效,由 UI 层持有)。
// 会话内冷却由调用方(会话结构 dr_event_session_t)记账,主机可测。
typedef struct {
    uint32_t last_fire_ts[128];       // 事件 id → 本会话上次触发时间戳
} dr_event_session_t;

void dr_event_session_init(dr_event_session_t *s);

// 返回触发的下标(*out_idx),或 DR_EVENT_NONE;同场景命中多个取 priority 最高、
// id 最小者(确定性,便于测试)。rng 使用 g 内种子派生的独立序列(不污染地图种子)。
dr_event_result_t dr_event_pick(const dr_event_t *events, uint16_t count,
                                const dr_game_t *g, dr_event_session_t *sess,
                                uint32_t now_ts, uint8_t scene,
                                dr_rng_t *rng, uint16_t *out_idx);

// 执行事件的某个选项(条件在 pick 时已判过)。应用效果并记账:
// event_count++、last_fire_ts 更新、RNG 状态写回 g。
// 返回 DR_EFF_GOTO_EVENT 链到的下一事件下标(无链返回 -1)。
int16_t dr_event_choose(const dr_event_t *events, uint16_t count,
                        dr_game_t *g, dr_event_session_t *sess,
                        uint32_t now_ts, uint16_t event_idx, uint8_t choice_idx);

#ifdef __cplusplus
}
#endif
