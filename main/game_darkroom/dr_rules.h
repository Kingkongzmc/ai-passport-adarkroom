// main/game_darkroom/dr_rules.h —— 规则层:火焰/温度/建造者剧情/采集/陷阱/人口/职业/贸易。
// 纯 C99,主机可测。数值与机制对齐原版 A Dark Room(源码取证见
// DESIGN.zh_CN.md §9);设备适配(三键 UI/NVS 存档/离线补算)单独标注。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr_state.h"
#include "dr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 剧情标记位(flags;事件表与规则层共用) ----
#define DR_FLAG_FOREST    2u   // 森林/村庄页解锁(原版 unlockForest)
#define DR_FLAG_COMPASS   3u   // 已购罗盘(原版 compass,M4 世界坐标)

// ---- 建筑(id 与 dr_game_t.building_lv 下标一致;布局红线:只尾插) ----
// 造价对齐原版 room.js Craftables;上限 = lv 上限。
typedef enum {
    DR_BLD_CART = 0,    // 板车:max1,30 木;采集 +50 木
    DR_BLD_TRAP,        // 陷阱:max10,10+10n 木
    DR_BLD_HUT,         // 小屋:max20,100+50n 木;每屋 +4 人口
    DR_BLD_LODGE,       // 猎人小屋:max1,200木+10毛+5肉;解锁猎人/捕兽人
    DR_BLD_TRADE_POST,  // 贸易站:max1,400木+100毛;解锁游牧商人(只买不卖)
    DR_BLD_TANNERY,     // 制革坊:max1,500木+50毛;解锁制革匠
    DR_BLD_SMOKEHOUSE,  // 熏肉房:max1,600木+50肉;解锁熏肉匠(v3 尾插)
    DR_BLD_STEELWORKS,  // 炼钢厂:max1,1500木+100铁+100煤;解锁炼钢工(v4 尾插)
    DR_BLD_ARMOURY,     // 军械库:max1,3000木+100钢+50硫;解锁军械工
    DR_BLD_WORKSHOP,    // 工坊:max1,800木+100革+10鳞;解锁制造(切片三尾插)
    DR_BLD_KIND_COUNT,
} dr_building_t;

// ---- 火焰(原版 FireEnum 5 档) ----
typedef enum {
    DR_FIRE_DEAD = 0,       // 熄灭
    DR_FIRE_SMOLDERING,     // 微弱
    DR_FIRE_FLICKERING,     // 跳动
    DR_FIRE_BURNING,        // 旺盛
    DR_FIRE_ROARING,        // 炽烈
} dr_fire_t;

// ---- 室温(原版 TempEnum 5 档;每 30s 向火焰档移动一步) ----
typedef enum {
    DR_TEMP_FREEZING = 0,   // 冻结
    DR_TEMP_COLD,           // 冷
    DR_TEMP_MILD,           // 微温
    DR_TEMP_WARM,           // 暖(建造者恢复需要)
    DR_TEMP_HOT,            // 热
} dr_temp_t;

// ---- 建造者(陌生人)状态机(原版 builder.level,-1 并入 0) ----
typedef enum {
    DR_BUILDER_NONE = 0,    // 未触发(火焰首次 ≥跳动 时进入 DR_BUILDER_DOWN)
    DR_BUILDER_DOWN,        // 晕倒;15s 后触发森林解锁剧情
    DR_BUILDER_SHIVER,      // 发抖(需室温 ≥暖,30s)
    DR_BUILDER_SLEEP,       // 沉睡(需室温 ≥暖,30s)
    DR_BUILDER_HELP,        // 帮忙:+2 木/10s 收入;火焰将熄时自动添柴
} dr_builder_t;

// 火焰衰减:每次点火/添柴后经此时长降一档(原版 _FIRE_COOL_DELAY 5 分钟);
// 降档瞬间若"建造者已帮忙 && 火焰 ≤跳动 && 有木" → 建造者先添 1 木 +1 档。
#define DR_FIRE_LEVEL_SECONDS  300u
// 室温调整周期(原版 _ROOM_WARM_DELAY 30s)。
#define DR_TEMP_LEVEL_SECONDS  30u
// 建造者状态推进间隔(原版 _BUILDER_STATE_DELAY 30s)与森林解锁延迟(15s)。
#define DR_BUILDER_STATE_SECONDS  30u
#define DR_FOREST_UNLOCK_S        15u

// ---- 手动动作(对齐原版 room.js/outside.js) ----
#define DR_GATHER_COOLDOWN_S   60u    // 采集木材冷却(原版 _GATHER_DELAY)
#define DR_GATHER_WOOD         10     // 每次采集 +10 木;有板车 +50(原版)
#define DR_STOKE_COOLDOWN_S    10u    // 点火/添柴共用冷却(原版 _STOKE_COOLDOWN)
#define DR_FIRE_LIGHT_COST     5      // 点火(从熄灭):5 木,直达"旺盛"
#define DR_FIRE_STOKE_COST     1      // 添柴(火燃着):1 木,+1 档封顶"炽烈"

// ---- 陷阱(原版 outside.js checkTraps) ----
#define DR_TRAP_PERIOD_S       90u    // 查看冷却(原版 _TRAPS_DELAY)
// 每件掉落(累积千分比):毛 50% / 肉 25% / 鳞 10% / 牙 8% / 布 6.5% / 护符 0.5%
#define DR_TRAP_P_FUR     500u
#define DR_TRAP_P_MEAT    750u
#define DR_TRAP_P_SCALES  850u
#define DR_TRAP_P_TEETH   930u
#define DR_TRAP_P_CLOTH   995u

// ---- 建造 ----
typedef struct {
    uint32_t wood;              // 0xFFFFFFFF = 不可建(满级/未实装)
    uint32_t fur, meat, iron, coal, steel, sulphur;   // 组件造价(原版口径)
    uint32_t leather, scales;   // 工坊等后期组件
} dr_bld_cost_t;
dr_bld_cost_t dr_building_cost(uint8_t building_id, uint8_t current_lv);

// ---- 查询 ----
dr_fire_t dr_rules_fire(const dr_game_t *g, uint32_t now_ts);
uint16_t  dr_rules_pop_cap(const dr_game_t *g);   // 小屋 ×4
bool      dr_rules_can_build(const dr_game_t *g, uint8_t building_id);
                                                // 建造者已帮忙 + 资源过半 + 材料见过
// ---- 动作(返回 false = 条件不满足,未改变状态) ----
bool dr_rules_stoke_fire(dr_game_t *g, uint32_t now_ts);     // 生火/添柴
bool dr_rules_gather(dr_game_t *g, uint32_t now_ts);         // 采集木材(+10/50)
bool dr_rules_build(dr_game_t *g, uint8_t building_id, uint32_t now_ts);

// ---- 周期结算:由 UI 的 1s 心跳驱动 ----
// 设备适配:计时基准不落盘,每次启动从 now_ms 重排;离线期火焰熄灭。
typedef struct {
    uint32_t fire_deadline_ms;   // 当前火焰档到期时刻
    uint32_t temp_next_ms;       // 下次室温调整时刻(30s)
    uint32_t builder_next_ms;    // 建造者状态推进时刻(30s)
    uint32_t forest_unlock_ms;   // 森林解锁时刻(晕倒后 15s;0=无待解锁)
    uint32_t gather_ready_ms;    // 采集冷却到期时刻
    uint32_t stoke_ready_ms;     // 点火/添柴冷却到期时刻
    uint32_t trap_next_ms;       // 下次陷阱可查看时刻
    uint32_t econ_next_ms;       // 下次收入结算时刻(10s)
    uint32_t wanderer_next_ms;   // 下次流浪者到达时刻(0.5~3min 随机)
    dr_rng_t rng;                // 陷阱/人口概率(从 g->rng_seed_state 续跑)
    bool     rng_inited;
    bool     hunter_parity;      // 猎人 0.5 产出相位(隔 tick 产一次)
} dr_rules_rt_t;

void dr_rules_rt_init(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms);
// 陌生人"沉睡→帮忙"的推进(原版发生在玩家回到房间时,onArrival):
// 进入小屋页时调用;仅在沉睡态生效,返回 true = 恢复为帮忙(应记日志)。
bool dr_rules_builder_visit(dr_game_t *g);
// 返回 true = 游戏数据有变化(需要重绘);*out_event 为剧情瞬间文案 id(dr_rules_event_t)。
typedef enum {
    DR_RT_EV_NONE = 0,
    DR_RT_EV_BUILDER_IN,     // 陌生人晕倒
    DR_RT_EV_FOREST,         // 森林解锁(柴火见底)
    DR_RT_EV_BUILDER_SHIVER, // 她打着寒战
    DR_RT_EV_BUILDER_SLEEP,  // 她不再发抖
    DR_RT_EV_BUILDER_HELP,   // 她可以帮忙了
    DR_RT_EV_BUILDER_STOKE,  // 建造者添了柴
    DR_RT_EV_FIRE_OUT,       // 火熄了
    DR_RT_EV_FIRE_DOWN,      // 火弱了一档
    DR_RT_EV_WANDERER,       // 流浪者到来(arg = 人数)
} dr_rules_event_t;
bool dr_rules_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms,
                   dr_rules_event_t *out_ev, uint16_t *out_arg);

// ---- 村庄职业(对齐原版 _INCOME;每 10s 结算) ----
typedef enum {
    DR_JOB_HUNTER = 0,    // 猎人(需猎人小屋):+0.5 毛 +0.5 肉/10s·人
    DR_JOB_TRAPPER,       // 捕兽人(需猎人小屋):−1 肉 → +1 饵/10s·人
    DR_JOB_TANNER,        // 制革匠(需制革坊):−5 毛 → +1 革/10s·人
    DR_JOB_CHARCUTIER,    // 熏肉匠(需熏肉房):−5 肉 −5 木 → +1 干肉/10s·人
    // M4(v4 尾插):矿工吃干肉产矿——到访对应矿并回家后解锁(原版 checkWorker)
    DR_JOB_IRON_MINER,    // 铁矿工:−1 干肉 → +1 铁/10s·人
    DR_JOB_COAL_MINER,    // 煤矿工:−1 干肉 → +1 煤/10s·人
    DR_JOB_SULPHUR_MINER, // 硫磺矿工:−1 干肉 → +1 硫/10s·人
    DR_JOB_STEELWORKER,   // 炼钢工(需炼钢厂):−1 铁 −1 煤 → +1 钢/10s·人
    DR_JOB_ARMOURER,      // 军械工(需军械库):−1 钢 −1 硫 → +1 子弹/10s·人
    DR_JOB_KIND_COUNT,
} dr_job_t;
// 采集者 = 未分配人口,+1 木/10s·人(原版 gatherer,无口粮系统——村民不吃东西)。
uint16_t dr_rules_job_idle(const dr_game_t *g);
bool     dr_rules_job_unlocked(const dr_game_t *g, uint8_t job);
// delta 正=分配/负=撤下;失败(超员/无人可撤/职业锁定)返回 false。
bool     dr_rules_job_assign(dr_game_t *g, uint8_t job, int16_t delta);

// ---- 流浪者(原版 increasePopulation;仅小屋有空间时) ----
// 到达间隔 0.5~3 分钟;人数 = floor(random*(余量/2)+余量/2),至少 1,钳到余量。
void     dr_rules_wanderer_schedule(dr_rules_rt_t *rt, uint32_t now_ms);

// ---- 离线结算(设备适配:原版无离线,本作按同一收入表逐 tick 补算) ----
typedef struct {
    uint32_t ticks;          // 等效收入 tick 数
    uint32_t wood, fur, meat, leather, food;   // 离线所得(已入账;food=干肉)
    bool     fire_out;       // 离线导致火焰熄灭
} dr_offline_yield_t;
uint32_t dr_rules_offline_settle(dr_rules_rt_t *rt, dr_game_t *g,
                                 uint32_t now_ts_s, uint32_t now_ms,
                                 dr_offline_yield_t *y);

// ---- 制造(原版 room.js crafts,§9.7;火把无需工坊,其余需工坊) ----
typedef enum {
    DR_CRAFT_TORCH = 0,     // 火把:1木+1布(消耗品,探索洞穴)
    DR_CRAFT_BONE_SPEAR,    // 骨矛:100木+5牙(武器2)
    DR_CRAFT_IRON_SWORD,    // 铁剑:200木+50革+20铁(武器4)
    DR_CRAFT_STEEL_SWORD,   // 钢剑:500木+100革+20钢(武器6)
    DR_CRAFT_RIFLE,         // 步枪:200木+50钢+50硫(武器5,耗弹)
    DR_CRAFT_L_ARMOUR,      // 皮甲:200革+20鳞(护甲1,HP+5)
    DR_CRAFT_I_ARMOUR,      // 铁甲:200革+100铁(护甲2,HP+15)
    DR_CRAFT_S_ARMOUR,      // 钢甲:200革+100钢(护甲3,HP+35)
    DR_CRAFT_WATERSKIN,     // 水袋:50革(水+10)
    DR_CRAFT_CASK,          // 木桶:100革+20铁(水+20)
    DR_CRAFT_TANK,          // 水箱:100铁+50钢(水+50)
    DR_CRAFT_RUCKSACK,      // 背囊:200革(背袋+10)
    DR_CRAFT_WAGON,         // 篷车:500木+100铁(背袋+30)
    DR_CRAFT_CONVOY,        // 车队:1000木+200铁+100钢(背袋+60)
    DR_CRAFT_KIND_COUNT,
} dr_craft_t;
bool dr_rules_craft(dr_game_t *g, uint8_t craft);        // 制造(含工坊门槛)
bool dr_rules_craft_owned(const dr_game_t *g, uint8_t craft);
// 制造可用(可见性):工坊/火把规则 + 材料见过 + 武器护甲按阶可见
bool dr_rules_craft_visible(const dr_game_t *g, uint8_t craft);
// 某制造项对某资源的材料数(UI 展示)
uint32_t dr_rules_craft_need(const dr_game_t *g, uint8_t craft, uint8_t res);
// 可立即制造:可见 + (未拥有,或火把) + 材料足额(UI 行可选判定)
bool dr_rules_craft_ready(const dr_game_t *g, uint8_t craft);

// ---- 贸易(原版 TradeGoods:游牧商人只买不卖,以毛/鳞/牙支付) ----
typedef enum {
    DR_TRADE_SCALES = 0,  // 鳞:150 毛
    DR_TRADE_TEETH,       // 牙:300 毛
    DR_TRADE_IRON,        // 铁:150 毛 + 50 鳞
    DR_TRADE_COAL,        // 煤:200 毛 + 50 牙
    DR_TRADE_STEEL,       // 钢:300 毛 + 50 鳞 + 50 牙
    DR_TRADE_BULLETS,     // 子弹:10 鳞
    DR_TRADE_MEDICINE,    // 药:50 鳞 + 30 牙(v4 尾插;远征治疗 +20HP)
    DR_TRADE_COMPASS,     // 罗盘:400 毛 + 20 鳞 + 10 牙(限 1;M4 世界坐标)
    DR_TRADE_KIND_COUNT,
} dr_trade_t;
bool dr_rules_trade_buy(dr_game_t *g, uint8_t item);   // 需已建贸易站

// ---- 陷阱:手动查看(原版 checkTraps) ----
// 冷却走满后结算:掉落件数 = 陷阱数 + min(饵, 陷阱数)(饵自动消耗,原版无开关);
// 每件按六档表掷出。out 返回各类数量;返回 false = 不在收获窗口/无陷阱。
typedef struct {
    uint8_t fur, meat, scales, teeth, cloth, charm;
    uint8_t bait;      // 本次消耗诱饵数
} dr_trap_yield_t;
bool dr_rules_trap_check(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms,
                         dr_trap_yield_t *out);
bool dr_rules_trap_ready(const dr_rules_rt_t *rt, const dr_game_t *g,
                         uint32_t now_ms);

#ifdef __cplusplus
}
#endif
