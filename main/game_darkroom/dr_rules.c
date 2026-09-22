// main/game_darkroom/dr_rules.c —— 第一幕经济规则的实现。
#include "dr_rules.h"
#include "dr_config.h"
#include "dr_util.h"

uint16_t dr_building_cost_wood(uint8_t building_id, uint8_t current_lv) {
    // 首件造价 + 每级翻倍(占位曲线,M3 调平):
    //   板车 10 / 陷阱 15(可叠)/ 小屋 20(可叠)/ 猎人小屋 150 / 贸易站 300
    switch ((dr_building_t)building_id) {
        case DR_BLD_CART:       return 10u << current_lv;
        case DR_BLD_TRAP:       return 15u << (current_lv > 3 ? 3 : current_lv);
        case DR_BLD_HUT:        return 20u << (current_lv > 4 ? 4 : current_lv);
        case DR_BLD_LODGE:      return 150u;
        case DR_BLD_TRADE_POST: return 300u;
        default:                return 0xFFFFu;  // 不可建
    }
}

// 额外成本:猎人小屋要 毛+肉,贸易站要 毛(对齐原版 lodge/trading post 口径)。
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
    // 前置:陷阱需要板车;小屋需要陷阱;猎人小屋/贸易站需要小屋。
    switch ((dr_building_t)building_id) {
        case DR_BLD_TRAP:
            return g->building_lv[DR_BLD_CART] > 0;
        case DR_BLD_HUT:
            return g->building_lv[DR_BLD_TRAP] > 0;
        case DR_BLD_LODGE:
        case DR_BLD_TRADE_POST:
            return g->building_lv[DR_BLD_HUT] > 0;
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
    // 无条件从档内种子派生:存档时 rng 已回写,重启后从断点续读同一序列
    dr_rng_seed(&rt->rng, g->rng_seed_state ^ 0xA5A5A5A5u);
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
    // 条件:屋里有人住、火烧着、有空闲房源
    if (g->population == 0 || g->fire_lv == DR_FIRE_DEAD ||
        g->population >= cap)
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
        default: return false;   // 制革匠/铁匠:M3+ 工作场所
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

// ---- 离线结算 ----
uint32_t dr_rules_offline_settle(dr_rules_rt_t *rt, dr_game_t *g,
                                 uint32_t now_ts_s, uint32_t now_ms,
                                 dr_offline_yield_t *y) {
    (void)now_ms;   // rt 各计时基准已由 rt_init 按 now_ms 归位
    dr_offline_yield_t zero = {0, 0, 0, 0, 0, false};
    if (y) *y = zero;
    if (now_ts_s <= g->saved_at_ts) return 0;   // 时钟回拨/无间隔:不结算
    uint32_t delta_s = now_ts_s - g->saved_at_ts;

    // 火焰:离线即熄(计时基准不落盘)
    if (g->fire_lv > DR_FIRE_DEAD) {
        g->fire_lv = DR_FIRE_DEAD;
        if (y) y->fire_out = true;
    }

    // 村庄产出:按等效经济 tick 补(8h 封顶,超出按 25% 折算)
    uint32_t ticks = dr_offline_ticks(g, now_ts_s);
    if (ticks) {
        uint32_t wood = ((uint32_t)dr_rules_job_idle(g) +
                         (uint32_t)g->job[DR_JOB_LUMBER] * 2u) * ticks;
        uint32_t fur = (uint32_t)g->job[DR_JOB_HUNTER] * ticks;
        g->res[DR_RES_WOOD] += wood;
        g->res_total[DR_RES_WOOD] += wood;
        if (fur) {
            g->res[DR_RES_FUR] += fur;
            g->res_total[DR_RES_FUR] += fur;
            g->res[DR_RES_MEAT] += fur;
            g->res_total[DR_RES_MEAT] += fur;
        }
        if (y) { y->ticks = ticks; y->wood = wood; y->fur = fur; y->meat = fur; }
    }

    // 陷阱:每个完整周期掷一次(与"查看陷阱"同概率;火已熄 → 减半)
    if (g->building_lv[DR_BLD_TRAP] > 0) {
        uint32_t cap_s = delta_s > DR_OFFLINE_CAP_S ? DR_OFFLINE_CAP_S : delta_s;
        uint32_t periods = cap_s / DR_TRAP_PERIOD_S;
        uint16_t chance = DR_TRAP_CHANCE_PERMILLE *
                          (uint16_t)g->building_lv[DR_BLD_TRAP];
        if (chance > 1000) chance = 1000;
        for (uint32_t i = 0; i < periods; i++) {
            if (!dr_rng_chance(&rt->rng, chance)) continue;
            if (dr_rng_chance(&rt->rng, 500)) {
                g->res[DR_RES_FUR] += 1;
                g->res_total[DR_RES_FUR] += 1;
                if (y) y->fur += 1;
            } else {
                g->res[DR_RES_MEAT] += 1;
                g->res_total[DR_RES_MEAT] += 1;
                if (y) y->meat += 1;
            }
            g->res[DR_RES_BAIT] += 1;
            g->res_total[DR_RES_BAIT] += 1;
            if (y) y->bait += 1;
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

    // 村庄产出:每 10s 经济 tick。采集者(未分配人口)+1 木;伐木工 +2 木;
    // 猎人 +1 毛 +1 肉。口粮消耗与制革/锻造链 M3+ 接入。
    if ((int32_t)(now_ms - rt->econ_next_ms) >= 0) {
        rt->econ_next_ms = now_ms + DR_ECONOMY_TICK_S * 1000u;
        uint32_t wood = (uint32_t)dr_rules_job_idle(g) +
                        (uint32_t)g->job[DR_JOB_LUMBER] * 2u;
        uint32_t fur = g->job[DR_JOB_HUNTER];
        uint32_t meat = fur;
        g->res[DR_RES_WOOD] += wood;
        g->res_total[DR_RES_WOOD] += wood;
        if (fur) {
            g->res[DR_RES_FUR] += fur;
            g->res_total[DR_RES_FUR] += fur;
            g->res[DR_RES_MEAT] += meat;
            g->res_total[DR_RES_MEAT] += meat;
        }
        changed = true;
    }

    // 流浪者:有房有火则随机到达
    if (dr_rules_wanderer_tick(rt, g, now_ms)) changed = true;

    // 陷阱:不再自动结算 —— 倒计时走满后由玩家"查看陷阱"手动收获
    // (冷却到期只停在 ready 态,收获逻辑在 dr_rules_trap_check)。
    return changed;
}

uint8_t dr_rules_trap_check(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms) {
    if (g->building_lv[DR_BLD_TRAP] == 0) return 0;
    if ((int32_t)(rt->trap_next_ms - now_ms) > 0) return 0;  // 还在冷却

    rt->trap_next_ms = now_ms + DR_TRAP_PERIOD_S * 1000u;
    uint16_t chance = DR_TRAP_CHANCE_PERMILLE *
                      (uint16_t)g->building_lv[DR_BLD_TRAP];
    if (g->fire_lv == DR_FIRE_DEAD) chance /= 2;
    if (chance > 1000) chance = 1000;

    uint8_t got = 0;
    if (dr_rng_chance(&rt->rng, chance)) {
        if (dr_rng_chance(&rt->rng, 500)) {
            g->res[DR_RES_FUR] += 1;
            g->res_total[DR_RES_FUR] += 1;
            got |= DR_TRAP_GOT_FUR;
        } else {
            g->res[DR_RES_MEAT] += 1;
            g->res_total[DR_RES_MEAT] += 1;
            got |= DR_TRAP_GOT_MEAT;
        }
        g->res[DR_RES_BAIT] += 1;
        g->res_total[DR_RES_BAIT] += 1;
        got |= DR_TRAP_GOT_BAIT;
    }
    g->rng_seed_state = rt->rng.s;  // 无论成败,序列推进都写回档
    return got;
}

bool dr_rules_trap_ready(const dr_rules_rt_t *rt, const dr_game_t *g,
                         uint32_t now_ms) {
    return g->building_lv[DR_BLD_TRAP] > 0 &&
           (int32_t)(now_ms - rt->trap_next_ms) >= 0;
}
