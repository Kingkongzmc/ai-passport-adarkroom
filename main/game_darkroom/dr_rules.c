// main/game_darkroom/dr_rules.c —— 第一/二幕经济规则的实现。
#include <string.h>

#include "dr_rules.h"
#include "dr_config.h"
#include "dr_util.h"

uint16_t dr_building_cost_wood(uint8_t building_id, uint8_t current_lv) {
    // 首件造价 + 每级翻倍(占位曲线,M3 调平):
    //   板车 10 / 陷阱 15(可叠)/ 小屋 20(可叠)/ 猎人小屋 150 / 贸易站 300 / 制革坊 120
    switch ((dr_building_t)building_id) {
        case DR_BLD_CART:       return 10u << current_lv;
        case DR_BLD_TRAP:       return 15u << (current_lv > 3 ? 3 : current_lv);
        case DR_BLD_HUT:        return 20u << (current_lv > 4 ? 4 : current_lv);
        case DR_BLD_LODGE:      return 150u;
        case DR_BLD_TRADE_POST: return 300u;
        case DR_BLD_TANNERY:    return 120u;
        default:                return 0xFFFFu;  // 不可建
    }
}

// 额外成本:猎人小屋要 毛+肉,贸易站/制革坊要 毛(对齐原版口径)。
// 每升一级按 (lv+1) 倍增长。
dr_bld_cost_t dr_building_cost(uint8_t building_id, uint8_t current_lv) {
    dr_bld_cost_t c = { dr_building_cost_wood(building_id, current_lv), 0, 0 };
    switch ((dr_building_t)building_id) {
        case DR_BLD_LODGE:
            c.fur = (uint16_t)(10u * (current_lv + 1u));
            c.meat = (uint16_t)(5u * (current_lv + 1u));
            break;
        case DR_BLD_TRADE_POST:
            c.fur = (uint16_t)(20u * (current_lv + 1u));
            break;
        case DR_BLD_TANNERY:
            c.fur = (uint16_t)(10u * (current_lv + 1u));
            break;
        default:
            break;
    }
    if (c.wood == 0xFFFFu) c.wood = 0xFFFFu;
    return c;
}

dr_fire_t dr_rules_fire(const dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;  // 档位存盘;计时到期由 tick 处理
    return (g->fire_lv > DR_FIRE_ROARING) ? DR_FIRE_ROARING : (dr_fire_t)g->fire_lv;
}

uint16_t dr_rules_pop_cap(const dr_game_t *g) {
    return (uint16_t)(g->building_lv[DR_BLD_HUT] * 2u);
}

uint32_t dr_rules_gather_ready_in(const dr_game_t *g, uint32_t now_ts) {
    (void)g; (void)now_ts;
    return 0;  // 冷却在运行时(rt->gather_ready_ms),UI 直接读 rt
}

bool dr_rules_can_build(const dr_game_t *g, uint8_t building_id) {
    if (building_id >= DR_BLD_KIND_COUNT) return false;
    uint8_t lv = g->building_lv[building_id];
    dr_bld_cost_t cost = dr_building_cost(building_id, lv);
    if (cost.wood == 0xFFFFu) return false;
    if (g->res[DR_RES_WOOD] < cost.wood) return false;
    if (g->res[DR_RES_FUR] < cost.fur) return false;
    if (g->res[DR_RES_MEAT] < cost.meat) return false;
    // 前置:陷阱需要板车;小屋需要陷阱;猎人小屋/贸易站需要小屋;制革坊需要猎人小屋。
    switch ((dr_building_t)building_id) {
        case DR_BLD_TRAP:
            return g->building_lv[DR_BLD_CART] > 0;
        case DR_BLD_HUT:
            return g->building_lv[DR_BLD_TRAP] > 0;
        case DR_BLD_LODGE:
        case DR_BLD_TRADE_POST:
            return g->building_lv[DR_BLD_HUT] > 0;
        case DR_BLD_TANNERY:
            return g->building_lv[DR_BLD_LODGE] > 0;
        default:
            return true;  // 板车无前置
    }
}

bool dr_rules_stoke_fire(dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;
    if (g->fire_lv == DR_FIRE_DEAD) {
        // 点火:5 木,直接到"旺盛"(原版 lightFire)
        if (g->res[DR_RES_WOOD] < DR_FIRE_LIGHT_COST) return false;
        g->res[DR_RES_WOOD] -= DR_FIRE_LIGHT_COST;
        g->fire_lv = DR_FIRE_BURNING;
    } else {
        // 添柴:1 木,+1 档封顶"炽烈"(原版 stokeFire)
        if (g->res[DR_RES_WOOD] < DR_FIRE_STOKE_COST) return false;
        g->res[DR_RES_WOOD] -= DR_FIRE_STOKE_COST;
        if (g->fire_lv < DR_FIRE_ROARING) g->fire_lv++;
    }
    return true;
}

bool dr_rules_gather(dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;
    g->res[DR_RES_WOOD] += DR_GATHER_WOOD;
    g->res_total[DR_RES_WOOD] += DR_GATHER_WOOD;
    return true;
}

bool dr_rules_build(dr_game_t *g, uint8_t building_id, uint32_t now_ts) {
    (void)now_ts;
    if (!dr_rules_can_build(g, building_id)) return false;
    uint8_t lv = g->building_lv[building_id];
    dr_bld_cost_t cost = dr_building_cost(building_id, lv);
    g->res[DR_RES_WOOD] -= cost.wood;
    g->res[DR_RES_FUR] -= cost.fur;
    g->res[DR_RES_MEAT] -= cost.meat;
    g->building_lv[building_id] = lv + 1;
    // 小屋建成 → 流浪者定居(占位:每级直接 +2 人口,直至人口上限)
    if (building_id == DR_BLD_HUT) {
        uint16_t cap = dr_rules_pop_cap(g);
        if (g->population + 2 <= cap) g->population += 2;
        else g->population = cap;
    }
    return true;
}

void dr_rules_rt_init(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms) {
    // 从档内 RNG 状态直接续跑:trap_check 每次结算把推进后的状态回写档,
    // 重启后严格续读同一序列(事件触发会改写该状态,属正常扰动)。
    dr_rng_seed(&rt->rng, g->rng_seed_state);
    rt->rng_inited = true;
    rt->fire_deadline_ms = now_ms + DR_FIRE_LEVEL_SECONDS * 1000u;
    rt->gather_ready_ms = now_ms;
    rt->stoke_ready_ms = now_ms;
    rt->trap_next_ms = now_ms + DR_TRAP_PERIOD_S * 1000u;
    rt->econ_next_ms = now_ms + DR_ECONOMY_TICK_S * 1000u;
    dr_rules_wanderer_schedule(rt, now_ms);
}

// ---- 流浪者到达:有房有火则 0.5~3 分钟来 1 人(原版 _POP_DELAY) ----
#define DR_WANDERER_MIN_S  30u
#define DR_WANDERER_MAX_S  180u

void dr_rules_wanderer_schedule(dr_rules_rt_t *rt, uint32_t now_ms) {
    uint32_t span = DR_WANDERER_MAX_S - DR_WANDERER_MIN_S;
    uint32_t r = rt->rng_inited ? dr_rng_next(&rt->rng) % (span + 1u) : 0u;
    rt->wanderer_next_ms = now_ms + (DR_WANDERER_MIN_S + r) * 1000u;
}

bool dr_rules_wanderer_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms) {
    if ((int32_t)(now_ms - rt->wanderer_next_ms) < 0) return false;
    dr_rules_wanderer_schedule(rt, now_ms);
    uint16_t cap = dr_rules_pop_cap(g);
    // 条件:屋里有人住、火烧着、有空闲房源、村里不断粮(荒年不收人)
    if (g->population == 0 || g->fire_lv == DR_FIRE_DEAD ||
        g->population >= cap || dr_rules_starving(g))
        return false;
    g->population++;
    return true;
}

// ---- 村庄职业 ----
uint16_t dr_rules_job_idle(const dr_game_t *g) {
    uint32_t assigned = 0;
    for (int i = 0; i < DR_JOB_KIND_COUNT; i++) assigned += g->job[i];
    return (g->population > assigned) ? (uint16_t)(g->population - assigned) : 0;
}

bool dr_rules_job_unlocked(const dr_game_t *g, uint8_t job) {
    switch ((dr_job_t)job) {
        case DR_JOB_LUMBER: return true;
        case DR_JOB_HUNTER: return g->building_lv[DR_BLD_LODGE] > 0;
        case DR_JOB_TANNER: return g->building_lv[DR_BLD_TANNERY] > 0;
        default: return false;   // 铁匠:M4(铁来源在荒野)
    }
}

bool dr_rules_job_assign(dr_game_t *g, uint8_t job, int16_t delta) {
    if (job >= DR_JOB_KIND_COUNT || delta == 0) return false;
    if (delta > 0 && !dr_rules_job_unlocked(g, job)) return false;
    int32_t v = (int32_t)g->job[job] + delta;
    if (v < 0) v = 0;                                   // 长按±5 的钳制语义
    if (delta > 0 && dr_rules_job_idle(g) < (uint16_t)delta) return false;
    g->job[job] = (uint16_t)v;
    return true;
}

bool dr_rules_starving(const dr_game_t *g) {
    return g->population > 0 &&
           g->res[DR_RES_FOOD] == 0 && g->res[DR_RES_MEAT] == 0;
}

// ---- 经济 tick 核心(在线心跳与离线补算共用) ----
// 顺序:先吃后产。口粮 = 人口 × DR_FOOD_PER_VILLAGER,先扣食物再扣肉;
// 两皆空 → 罢工:岗位全部停工,闲人只拾荒求生(+1 食,无木)。
typedef struct {
    uint32_t wood, food, fur, meat, leather;
    uint32_t food_eaten;
    bool     starving;
} dr_econ_out_t;

static void econ_step(dr_game_t *g, dr_econ_out_t *out) {
    memset(out, 0, sizeof(*out));
    uint16_t pop = g->population;
    if (pop == 0) return;                        // 无人:无产出也无口粮

    uint32_t need = (uint32_t)pop * DR_FOOD_PER_VILLAGER;
    if ((uint64_t)g->res[DR_RES_FOOD] + (uint64_t)g->res[DR_RES_MEAT] < need) {
        // 断粮:吃光存量;职业停工,闲人拾荒求生(只产食物,不出木)
        out->starving = true;
        out->food_eaten = g->res[DR_RES_FOOD] + g->res[DR_RES_MEAT];
        g->res[DR_RES_FOOD] = 0;
        g->res[DR_RES_MEAT] = 0;
        uint32_t scavenge = (uint32_t)dr_rules_job_idle(g) * DR_GATHERER_FOOD;
        g->res[DR_RES_FOOD] += scavenge;
        g->res_total[DR_RES_FOOD] += scavenge;
        out->food = scavenge;
        return;
    }
    uint32_t from_food = (g->res[DR_RES_FOOD] >= need) ? need
                                                       : g->res[DR_RES_FOOD];
    g->res[DR_RES_FOOD] -= from_food;
    g->res[DR_RES_MEAT] -= need - from_food;
    out->food_eaten = need;

    // 产出:采集者 +1木+1食;伐木工 +2木;猎人 +1毛+2肉;制革匠 2毛→1革(毛不足空转)
    uint16_t idle = dr_rules_job_idle(g);
    uint32_t wood = (uint32_t)idle + (uint32_t)g->job[DR_JOB_LUMBER] * 2u;
    uint32_t food = (uint32_t)idle * DR_GATHERER_FOOD;
    uint32_t hunt = g->job[DR_JOB_HUNTER];
    uint32_t fur = hunt, meat = hunt * DR_HUNTER_MEAT;
    g->res[DR_RES_WOOD] += wood;   g->res_total[DR_RES_WOOD] += wood;
    g->res[DR_RES_FOOD] += food;   g->res_total[DR_RES_FOOD] += food;
    if (fur)  { g->res[DR_RES_FUR] += fur;   g->res_total[DR_RES_FUR] += fur; }
    if (meat) { g->res[DR_RES_MEAT] += meat; g->res_total[DR_RES_MEAT] += meat; }
    uint16_t tanners = g->job[DR_JOB_TANNER];
    while (tanners-- > 0 && g->res[DR_RES_FUR] >= DR_TANNER_FUR) {
        g->res[DR_RES_FUR] -= DR_TANNER_FUR;
        g->res[DR_RES_LEATHER] += 1;
        g->res_total[DR_RES_LEATHER] += 1;
        out->leather += 1;
    }
    out->wood = wood; out->food = food; out->fur = fur; out->meat = meat;
}

// ---- 贸易:全部卖出 ----
#define DR_TRADE_FUR_WOOD  5u
#define DR_TRADE_MEAT_WOOD 3u

uint32_t dr_rules_trade_sell_value(const dr_game_t *g) {
    return (uint32_t)g->res[DR_RES_FUR] * DR_TRADE_FUR_WOOD +
           (uint32_t)g->res[DR_RES_MEAT] * DR_TRADE_MEAT_WOOD;
}

uint32_t dr_rules_trade_sell_all(dr_game_t *g) {
    uint32_t gain = dr_rules_trade_sell_value(g);
    if (gain == 0) return 0;
    g->res[DR_RES_WOOD] += gain;
    g->res_total[DR_RES_WOOD] += gain;
    g->res[DR_RES_FUR] = 0;
    g->res[DR_RES_MEAT] = 0;
    return gain;
}

// 整数组交易:资源足够才成交(计入累计获得)。
bool dr_rules_trade_fur10(dr_game_t *g) {
    if (g->res[DR_RES_FUR] < 10) return false;
    g->res[DR_RES_FUR] -= 10;
    g->res[DR_RES_WOOD] += 10u * DR_TRADE_FUR_WOOD;
    g->res_total[DR_RES_WOOD] += 10u * DR_TRADE_FUR_WOOD;
    return true;
}

bool dr_rules_trade_meat10(dr_game_t *g) {
    if (g->res[DR_RES_MEAT] < 10) return false;
    g->res[DR_RES_MEAT] -= 10;
    g->res[DR_RES_WOOD] += 10u * DR_TRADE_MEAT_WOOD;
    g->res_total[DR_RES_WOOD] += 10u * DR_TRADE_MEAT_WOOD;
    return true;
}

bool dr_rules_trade_bait5(dr_game_t *g) {
    if (g->res[DR_RES_WOOD] < 5u * DR_TRADE_BAIT_WOOD) return false;
    g->res[DR_RES_WOOD] -= 5u * DR_TRADE_BAIT_WOOD;
    g->res[DR_RES_BAIT] += 5;
    g->res_total[DR_RES_BAIT] += 5;
    return true;
}

bool dr_rules_trade_armor(dr_game_t *g) {
    if (g->armor_lv > 0) return false;                 // 已穿着
    if (g->res[DR_RES_WOOD] < DR_TRADE_ARMOR_WOOD) return false;
    if (g->res[DR_RES_LEATHER] < DR_TRADE_ARMOR_LEATHER) return false;
    g->res[DR_RES_WOOD] -= DR_TRADE_ARMOR_WOOD;
    g->res[DR_RES_LEATHER] -= DR_TRADE_ARMOR_LEATHER;
    g->armor_lv = 1;
    return true;
}

// ---- 离线结算 ----
uint32_t dr_rules_offline_settle(dr_rules_rt_t *rt, dr_game_t *g,
                                 uint32_t now_ts_s, uint32_t now_ms,
                                 dr_offline_yield_t *y) {
    (void)now_ms;   // rt 各计时基准已由 rt_init 按 now_ms 归位
    dr_offline_yield_t zero = {0, 0, 0, 0, 0, 0, false, false};
    if (y) *y = zero;
    if (now_ts_s < g->saved_at_ts) {          // 时钟回拨(RTC 丢失):锚定当下
        g->saved_at_ts = now_ts_s;            // 不结算,防止回拨期反复判负
        return 0;
    }
    if (now_ts_s == g->saved_at_ts) return 0; // 无间隔
    uint32_t delta_s = now_ts_s - g->saved_at_ts;

    // 火焰:离线即熄(计时基准不落盘)
    if (g->fire_lv > DR_FIRE_DEAD) {
        g->fire_lv = DR_FIRE_DEAD;
        if (y) y->fire_out = true;
    }

    // 村庄:按等效经济 tick 逐 tick 结算(含口粮消耗与罢工;
    // 8h 封顶,超出按 25% 折算——与在线完全同一条 econ_step 路径)
    uint32_t ticks = dr_offline_ticks(g, now_ts_s);
    if (y) y->ticks = ticks;
    for (uint32_t i = 0; i < ticks; i++) {
        dr_econ_out_t e;
        econ_step(g, &e);
        if (y) {
            y->wood += e.wood; y->fur += e.fur; y->meat += e.meat;
            y->food_eaten += e.food_eaten;
            if (e.starving) y->starving = true;
        }
    }

    // 陷阱:每个完整周期按"查看陷阱"同口径结算(每陷阱必得 1 件,毛/肉各半;
    // 火熄不影响——离线火必熄,与原版一致无惩罚)。
    // 离线不耗诱饵(诱饵只在手动查看时消耗,口径见 GAMEPLAY §3)。
    if (g->building_lv[DR_BLD_TRAP] > 0) {
        uint32_t cap_s = delta_s > DR_OFFLINE_CAP_S ? DR_OFFLINE_CAP_S : delta_s;
        uint32_t periods = cap_s / DR_TRAP_PERIOD_S;
        uint16_t rolls = g->building_lv[DR_BLD_TRAP];
        for (uint32_t i = 0; i < periods; i++) {
            for (uint16_t r = 0; r < rolls; r++) {
                if (dr_rng_chance(&rt->rng, DR_TRAP_FUR_PERMILLE)) {
                    g->res[DR_RES_FUR] += 1;
                    g->res_total[DR_RES_FUR] += 1;
                    if (y) y->fur += 1;
                } else {
                    g->res[DR_RES_MEAT] += 1;
                    g->res_total[DR_RES_MEAT] += 1;
                    if (y) y->meat += 1;
                }
            }
        }
    }
    return ticks;
}

bool dr_rules_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms, bool *fire_out) {
    bool changed = false;
    if (fire_out) *fire_out = false;

    // 火焰衰减
    if (g->fire_lv > DR_FIRE_DEAD && (int32_t)(now_ms - rt->fire_deadline_ms) >= 0) {
        g->fire_lv--;
        rt->fire_deadline_ms = now_ms + DR_FIRE_LEVEL_SECONDS * 1000u;
        changed = true;
        if (fire_out) *fire_out = true;
    }

    // 村庄:每 10s 一个经济 tick(口粮消耗 → 产出/罢工,见 econ_step)
    if ((int32_t)(now_ms - rt->econ_next_ms) >= 0) {
        rt->econ_next_ms = now_ms + DR_ECONOMY_TICK_S * 1000u;
        dr_econ_out_t e;
        econ_step(g, &e);
        changed = true;
    }

    // 流浪者:有房有火不断粮则随机到达
    if (dr_rules_wanderer_tick(rt, g, now_ms)) changed = true;

    // 陷阱:不再自动结算 —— 倒计时走满后由玩家"查看陷阱"手动收获
    // (冷却到期只停在 ready 态,收获逻辑在 dr_rules_trap_check)。
    return changed;
}

bool dr_rules_trap_check(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms,
                         dr_trap_yield_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (g->building_lv[DR_BLD_TRAP] == 0) return false;
    if ((int32_t)(rt->trap_next_ms - now_ms) > 0) return false;  // 还在冷却

    rt->trap_next_ms = now_ms + DR_TRAP_PERIOD_S * 1000u;

    // 原版 checkTraps:每陷阱必得 1 件(无命中率);诱饵开着先耗 min(饵,陷阱数),
    // 每耗 1 饵多掷 1 件。火熄与否不影响(原版无此设定)。
    uint16_t traps = g->building_lv[DR_BLD_TRAP];
    uint16_t rolls = traps;
    if (g->trap_bait_on && g->res[DR_RES_BAIT] > 0) {
        uint16_t bait = (g->res[DR_RES_BAIT] < traps) ? g->res[DR_RES_BAIT] : traps;
        g->res[DR_RES_BAIT] -= bait;
        rolls += bait;
        if (out) out->bait = (uint8_t)bait;
    }
    for (uint16_t r = 0; r < rolls; r++) {
        if (dr_rng_chance(&rt->rng, DR_TRAP_FUR_PERMILLE)) {
            g->res[DR_RES_FUR] += 1;
            g->res_total[DR_RES_FUR] += 1;
            if (out) out->fur++;
        } else {
            g->res[DR_RES_MEAT] += 1;
            g->res_total[DR_RES_MEAT] += 1;
            if (out) out->meat++;
        }
    }
    g->rng_seed_state = rt->rng.s;  // 序列推进写回档
    return true;
}

bool dr_rules_trap_ready(const dr_rules_rt_t *rt, const dr_game_t *g,
                         uint32_t now_ms) {
    return g->building_lv[DR_BLD_TRAP] > 0 &&
           (int32_t)(now_ms - rt->trap_next_ms) >= 0;
}
