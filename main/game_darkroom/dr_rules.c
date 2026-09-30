// main/game_darkroom/dr_rules.c —— 规则实现:对齐原版 A Dark Room。
// 取证来源见 DESIGN.zh_CN.md §9(room.js / outside.js / world.js / engine.js)。
#include <string.h>

#include "dr_rules.h"
#include "dr_config.h"
#include "dr_util.h"

// ---- 建筑造价(原版 room.js Craftables;wood=0xFFFFFFFF 表示不可再建) ----
static const uint32_t DR_COST_MAX = 0xFFFFFFFFu;

dr_bld_cost_t dr_building_cost(uint8_t building_id, uint8_t lv) {
    dr_bld_cost_t c = {0, 0, 0, 0, 0, 0, 0};
    switch ((dr_building_t)building_id) {
        case DR_BLD_CART:       // max 1
            c.wood = (lv >= 1) ? DR_COST_MAX : 30u;
            break;
        case DR_BLD_TRAP:       // max 10
            c.wood = (lv >= 10) ? DR_COST_MAX : 10u + 10u * lv;
            break;
        case DR_BLD_HUT:        // max 20
            c.wood = (lv >= 20) ? DR_COST_MAX : 100u + 50u * lv;
            break;
        case DR_BLD_LODGE:      // max 1
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 200u; c.fur = 10u; c.meat = 5u;
            break;
        case DR_BLD_TRADE_POST: // max 1
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 400u; c.fur = 100u;
            break;
        case DR_BLD_TANNERY:    // max 1
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 500u; c.fur = 50u;
            break;
        case DR_BLD_SMOKEHOUSE: // max 1
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 600u; c.meat = 50u;
            break;
        case DR_BLD_STEELWORKS: // max 1:1500 木 + 100 铁 + 100 煤
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 1500u; c.iron = 100u; c.coal = 100u;
            break;
        case DR_BLD_ARMOURY:    // max 1:3000 木 + 100 钢 + 50 硫
            if (lv >= 1) { c.wood = DR_COST_MAX; break; }
            c.wood = 3000u; c.steel = 100u; c.sulphur = 50u;
            break;
        default:
            c.wood = DR_COST_MAX;  // 未实装槽位(切片三:工坊)
            break;
    }
    return c;
}

dr_fire_t dr_rules_fire(const dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;
    return (g->fire_lv > DR_FIRE_ROARING) ? DR_FIRE_ROARING : (dr_fire_t)g->fire_lv;
}

uint16_t dr_rules_pop_cap(const dr_game_t *g) {
    return (uint16_t)(g->building_lv[DR_BLD_HUT] * 4u);   // 原版 _HUT_ROOM
}

// 建造可用(原版 craftUnlocked):建造者已帮忙 + 木材过半 + 造价材料"见过"。
bool dr_rules_can_build(const dr_game_t *g, uint8_t building_id) {
    if (building_id >= DR_BLD_KIND_COUNT) return false;
    if (g->builder_lv < DR_BUILDER_HELP) return false;
    uint8_t lv = g->building_lv[building_id];
    dr_bld_cost_t cost = dr_building_cost(building_id, lv);
    if (cost.wood == DR_COST_MAX) return false;
    if (g->res[DR_RES_WOOD] * 2u < cost.wood) return false;
    if (cost.fur > 0 && g->res[DR_RES_FUR] == 0) return false;
    if (cost.meat > 0 && g->res[DR_RES_MEAT] == 0) return false;
    if (cost.iron > 0 && g->res[DR_RES_IRON] == 0) return false;
    if (cost.coal > 0 && g->res[DR_RES_COAL] == 0) return false;
    if (cost.steel > 0 && g->res[DR_RES_STEEL] == 0) return false;
    if (cost.sulphur > 0 && g->res[DR_RES_SULPHUR] == 0) return false;
    return true;
}

bool dr_rules_stoke_fire(dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;
    if (g->fire_lv == DR_FIRE_DEAD) {
        if (g->res[DR_RES_WOOD] < DR_FIRE_LIGHT_COST) return false;
        g->res[DR_RES_WOOD] -= DR_FIRE_LIGHT_COST;
        g->fire_lv = DR_FIRE_BURNING;
    } else {
        if (g->res[DR_RES_WOOD] < DR_FIRE_STOKE_COST) return false;
        g->res[DR_RES_WOOD] -= DR_FIRE_STOKE_COST;
        if (g->fire_lv < DR_FIRE_ROARING) g->fire_lv++;
    }
    // 原版 onFireChange:火焰首次达到"跳动"且陌生人未出现 → 晕倒剧情启动。
    // (状态机推进由 dr_rules_tick 的定时器负责,这里只置入场。)
    return true;
}

bool dr_rules_gather(dr_game_t *g, uint32_t now_ts) {
    (void)now_ts;
    uint32_t amount = (g->building_lv[DR_BLD_CART] > 0) ? 50u : 10u;
    g->res[DR_RES_WOOD] += amount;
    g->res_total[DR_RES_WOOD] += amount;
    return true;
}

bool dr_rules_build(dr_game_t *g, uint8_t building_id, uint32_t now_ts) {
    (void)now_ts;
    if (!dr_rules_can_build(g, building_id)) return false;
    // 原版 build():室温须高于"冷"(她正打着寒战,没法帮忙)。
    if (g->temp_lv <= DR_TEMP_COLD) return false;
    uint8_t lv = g->building_lv[building_id];
    dr_bld_cost_t cost = dr_building_cost(building_id, lv);
    if (g->res[DR_RES_WOOD] < cost.wood) return false;
    if (g->res[DR_RES_FUR] < cost.fur) return false;
    if (g->res[DR_RES_MEAT] < cost.meat) return false;
    if (g->res[DR_RES_IRON] < cost.iron) return false;
    if (g->res[DR_RES_COAL] < cost.coal) return false;
    if (g->res[DR_RES_STEEL] < cost.steel) return false;
    if (g->res[DR_RES_SULPHUR] < cost.sulphur) return false;
    g->res[DR_RES_WOOD] -= cost.wood;
    g->res[DR_RES_FUR] -= cost.fur;
    g->res[DR_RES_MEAT] -= cost.meat;
    g->res[DR_RES_IRON] -= cost.iron;
    g->res[DR_RES_COAL] -= cost.coal;
    g->res[DR_RES_STEEL] -= cost.steel;
    g->res[DR_RES_SULPHUR] -= cost.sulphur;
    g->building_lv[building_id] = lv + 1;
    return true;   // 原版建房不加人口,人口只来自流浪者到达
}

void dr_rules_rt_init(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms) {
    memset(rt, 0, sizeof(*rt));
    // RNG 从档内状态直接续跑(trap_check 每次把推进后的状态回写档)
    dr_rng_seed(&rt->rng, g->rng_seed_state);
    rt->rng_inited = true;
    // 猎人半率相位落盘于 _rsv[0](原版收入引擎的小数累积是持久状态)
    rt->hunter_parity = (g->_rsv[0] != 0);
    rt->fire_deadline_ms = now_ms + DR_FIRE_LEVEL_SECONDS * 1000u;
    rt->temp_next_ms = now_ms + DR_TEMP_LEVEL_SECONDS * 1000u;
    rt->builder_next_ms = now_ms + DR_BUILDER_STATE_SECONDS * 1000u;
    rt->gather_ready_ms = now_ms;
    rt->stoke_ready_ms = now_ms;
    rt->trap_next_ms = now_ms + DR_TRAP_PERIOD_S * 1000u;
    rt->econ_next_ms = now_ms + DR_ECONOMY_TICK_S * 1000u;
    // 重启补偿:陌生人已晕倒但森林未解锁 → 15s 后补触发
    if (g->builder_lv == DR_BUILDER_DOWN && !(g->flags & ((uint64_t)1u << DR_FLAG_FOREST)))
        rt->forest_unlock_ms = now_ms + DR_FOREST_UNLOCK_S * 1000u;
    if (g->building_lv[DR_BLD_HUT] > 0)
        dr_rules_wanderer_schedule(rt, now_ms);
}

// ---- 流浪者到达(原版 increasePopulation):有房(有空间)则 0.5~3 分钟来一批 ----
#define DR_WANDERER_MIN_S  30u
#define DR_WANDERER_MAX_S  180u

void dr_rules_wanderer_schedule(dr_rules_rt_t *rt, uint32_t now_ms) {
    uint32_t span = DR_WANDERER_MAX_S - DR_WANDERER_MIN_S;
    uint32_t r = rt->rng_inited ? dr_rng_next(&rt->rng) % (span + 1u) : 0u;
    rt->wanderer_next_ms = now_ms + (DR_WANDERER_MIN_S + r) * 1000u;
}

// ---- 村庄职业 ----
uint16_t dr_rules_job_idle(const dr_game_t *g) {
    uint32_t assigned = 0;
    for (int i = 0; i < DR_JOB_KIND_COUNT; i++) assigned += g->job[i];
    return (g->population > assigned) ? (uint16_t)(g->population - assigned) : 0;
}

bool dr_rules_job_unlocked(const dr_game_t *g, uint8_t job) {
    switch ((dr_job_t)job) {
        case DR_JOB_HUNTER:     // 原版 checkWorker:猎人/捕兽人都挂猎人小屋
        case DR_JOB_TRAPPER:
            return g->building_lv[DR_BLD_LODGE] > 0;
        case DR_JOB_TANNER:
            return g->building_lv[DR_BLD_TANNERY] > 0;
        case DR_JOB_CHARCUTIER:
            return g->building_lv[DR_BLD_SMOKEHOUSE] > 0;
        case DR_JOB_IRON_MINER:     // 到访矿并回家(原版 checkWorker 挂矿建筑)
            return (g->flags & ((uint64_t)1u << DR_FLAG_IRON_MINE)) != 0;
        case DR_JOB_COAL_MINER:
            return (g->flags & ((uint64_t)1u << DR_FLAG_COAL_MINE)) != 0;
        case DR_JOB_SULPHUR_MINER:
            return (g->flags & ((uint64_t)1u << DR_FLAG_SULPHUR_MINE)) != 0;
        case DR_JOB_STEELWORKER:
            return g->building_lv[DR_BLD_STEELWORKS] > 0;
        case DR_JOB_ARMOURER:
            return g->building_lv[DR_BLD_ARMOURY] > 0;
        default:
            return false;
    }
}

bool dr_rules_job_assign(dr_game_t *g, uint8_t job, int16_t delta) {
    if (job >= DR_JOB_KIND_COUNT || delta == 0) return false;
    if (delta > 0 && !dr_rules_job_unlocked(g, job)) return false;
    int32_t v = (int32_t)g->job[job] + delta;
    if (v < 0) v = 0;
    if (delta > 0 && dr_rules_job_idle(g) < (uint16_t)delta) return false;
    g->job[job] = (uint16_t)v;
    return true;
}

// ---- 收入结算(原版 _INCOME,每 10s;村民不吃东西,无口粮/罢工) ----
// 猎人 0.5 毛 0.5 肉:用隔 tick 相位实现半速率(每 2 tick 产 1 次,均值即 0.5)。
typedef struct {
    uint32_t wood, fur, meat, leather, food;   // 毛产出(不含职业自身消耗)
} dr_income_out_t;

static void income_step(dr_rules_rt_t *rt, dr_game_t *g, dr_income_out_t *out) {
    memset(out, 0, sizeof(*out));

    uint32_t wood = dr_rules_job_idle(g);              // 采集者 +1 木/人
    if (g->builder_lv >= DR_BUILDER_HELP) wood += 2u;  // 建造者 +2 木(原版)

    rt->hunter_parity = !rt->hunter_parity;
    g->_rsv[0] = rt->hunter_parity ? 1u : 0u;          // 相位落盘(原版小数累积同义)
    uint32_t fur = 0, meat = 0;
    if (rt->hunter_parity) {                           // 猎人 +0.5 毛 +0.5 肉/人
        fur = g->job[DR_JOB_HUNTER];
        meat = g->job[DR_JOB_HUNTER];
    }

    uint32_t leather = 0, food = 0;
    // 捕兽人 −1 肉 → +1 饵(肉不够的部分空转)
    uint32_t conv = g->job[DR_JOB_TRAPPER];
    if (conv > g->res[DR_RES_MEAT]) conv = g->res[DR_RES_MEAT];
    g->res[DR_RES_MEAT] -= conv;
    g->res[DR_RES_BAIT] += conv;
    g->res_total[DR_RES_BAIT] += conv;
    // 矿工链(原版:−1 干肉 → +1 矿/10s·人;干肉不够空转)
    uint32_t cured = g->res[DR_RES_FOOD];
    struct { uint8_t job; uint8_t res; } miners[3] = {
        { DR_JOB_IRON_MINER, DR_RES_IRON },
        { DR_JOB_COAL_MINER, DR_RES_COAL },
        { DR_JOB_SULPHUR_MINER, DR_RES_SULPHUR },
    };
    for (int i = 0; i < 3; i++) {
        uint32_t n = g->job[miners[i].job];
        if (n > cured) n = cured;
        cured -= n;
        g->res[DR_RES_FOOD] -= n;
        g->res[miners[i].res] += n;
        g->res_total[miners[i].res] += n;
    }
    // 炼钢工 −1 铁 −1 煤 → +1 钢
    for (uint16_t i = 0; i < g->job[DR_JOB_STEELWORKER] &&
                         g->res[DR_RES_IRON] >= 1u && g->res[DR_RES_COAL] >= 1u; i++) {
        g->res[DR_RES_IRON] -= 1;
        g->res[DR_RES_COAL] -= 1;
        g->res[DR_RES_STEEL] += 1;
        g->res_total[DR_RES_STEEL] += 1;
    }
    // 军械工 −1 钢 −1 硫 → +1 子弹
    for (uint16_t i = 0; i < g->job[DR_JOB_ARMOURER] &&
                         g->res[DR_RES_STEEL] >= 1u && g->res[DR_RES_SULPHUR] >= 1u; i++) {
        g->res[DR_RES_STEEL] -= 1;
        g->res[DR_RES_SULPHUR] -= 1;
        g->res[DR_RES_BULLETS] += 1;
        g->res_total[DR_RES_BULLETS] += 1;
    }
    // 制革匠 −5 毛 → +1 革
    for (uint16_t i = 0; i < g->job[DR_JOB_TANNER] &&
                         g->res[DR_RES_FUR] >= 5u; i++) {
        g->res[DR_RES_FUR] -= 5u;
        g->res[DR_RES_LEATHER] += 1;
        g->res_total[DR_RES_LEATHER] += 1;
        leather++;
    }
    // 熏肉匠 −5 肉 −5 木 → +1 干肉
    for (uint16_t i = 0; i < g->job[DR_JOB_CHARCUTIER] &&
                         g->res[DR_RES_MEAT] >= 5u && g->res[DR_RES_WOOD] >= 5u; i++) {
        g->res[DR_RES_MEAT] -= 5u;
        g->res[DR_RES_WOOD] -= 5u;
        g->res[DR_RES_FOOD] += 1;
        g->res_total[DR_RES_FOOD] += 1;
        food++;
    }

    g->res[DR_RES_WOOD] += wood;   g->res_total[DR_RES_WOOD] += wood;
    g->res[DR_RES_FUR] += fur;     g->res_total[DR_RES_FUR] += fur;
    g->res[DR_RES_MEAT] += meat;   g->res_total[DR_RES_MEAT] += meat;
    out->wood = wood; out->fur = fur; out->meat = meat;
    out->leather = leather; out->food = food;
}

// ---- 陷阱掉落(原版 checkTraps 六档累积表) ----
static void trap_drop(dr_game_t *g, uint32_t roll) {
    if (roll < DR_TRAP_P_FUR) {
        g->res[DR_RES_FUR]++;     g->res_total[DR_RES_FUR]++;
    } else if (roll < DR_TRAP_P_MEAT) {
        g->res[DR_RES_MEAT]++;    g->res_total[DR_RES_MEAT]++;
    } else if (roll < DR_TRAP_P_SCALES) {
        g->res[DR_RES_SCALES]++;  g->res_total[DR_RES_SCALES]++;
    } else if (roll < DR_TRAP_P_TEETH) {
        g->res[DR_RES_TEETH]++;   g->res_total[DR_RES_TEETH]++;
    } else if (roll < DR_TRAP_P_CLOTH) {
        g->res[DR_RES_CLOTH]++;   g->res_total[DR_RES_CLOTH]++;
    } else {
        g->res[DR_RES_CHARM]++;   g->res_total[DR_RES_CHARM]++;
    }
}

static void trap_yield_add(dr_trap_yield_t *y, uint32_t roll) {
    if (roll < DR_TRAP_P_FUR) y->fur++;
    else if (roll < DR_TRAP_P_MEAT) y->meat++;
    else if (roll < DR_TRAP_P_SCALES) y->scales++;
    else if (roll < DR_TRAP_P_TEETH) y->teeth++;
    else if (roll < DR_TRAP_P_CLOTH) y->cloth++;
    else y->charm++;
}

bool dr_rules_trap_check(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms,
                         dr_trap_yield_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (g->building_lv[DR_BLD_TRAP] == 0) return false;
    if ((int32_t)(rt->trap_next_ms - now_ms) > 0) return false;  // 还在冷却

    rt->trap_next_ms = now_ms + DR_TRAP_PERIOD_S * 1000u;

    // 原版:numDrops = 陷阱数 + min(饵, 陷阱数);饵自动消耗,无开关。
    uint32_t traps = g->building_lv[DR_BLD_TRAP];
    uint32_t rolls = traps;
    if (g->res[DR_RES_BAIT] > 0) {
        uint32_t bait = (g->res[DR_RES_BAIT] < traps) ? g->res[DR_RES_BAIT] : traps;
        g->res[DR_RES_BAIT] -= bait;
        rolls += bait;
        if (out) out->bait = (uint8_t)bait;
    }
    for (uint32_t r = 0; r < rolls; r++) {
        uint32_t roll = dr_rng_below(&rt->rng, 1000u);
        trap_drop(g, roll);
        if (out) trap_yield_add(out, roll);
    }
    g->rng_seed_state = rt->rng.s;  // 序列推进写回档
    return true;
}

bool dr_rules_trap_ready(const dr_rules_rt_t *rt, const dr_game_t *g,
                         uint32_t now_ms) {
    return g->building_lv[DR_BLD_TRAP] > 0 &&
           (int32_t)(now_ms - rt->trap_next_ms) >= 0;
}

// ---- 贸易(原版 TradeGoods:只买不卖) ----
typedef struct {
    uint32_t fur, scales, teeth;
} dr_trade_cost_t;
static const dr_trade_cost_t k_trade_cost[DR_TRADE_KIND_COUNT] = {
    [DR_TRADE_SCALES] = { 150, 0, 0 },
    [DR_TRADE_TEETH]  = { 300, 0, 0 },
    [DR_TRADE_IRON]   = { 150, 50, 0 },
    [DR_TRADE_COAL]   = { 200, 0, 50 },
    [DR_TRADE_STEEL]  = { 300, 50, 50 },
    [DR_TRADE_BULLETS]= { 0, 10, 0 },
    [DR_TRADE_MEDICINE]= { 0, 50, 30 },
    [DR_TRADE_COMPASS]= { 400, 20, 10 },
};

bool dr_rules_trade_buy(dr_game_t *g, uint8_t item) {
    if (item >= DR_TRADE_KIND_COUNT) return false;
    if (g->building_lv[DR_BLD_TRADE_POST] == 0) return false;
    if (item == DR_TRADE_COMPASS &&
        (g->flags & ((uint64_t)1u << DR_FLAG_COMPASS))) return false;  // 限 1
    const dr_trade_cost_t *c = &k_trade_cost[item];
    if (g->res[DR_RES_FUR] < c->fur) return false;
    if (g->res[DR_RES_SCALES] < c->scales) return false;
    if (g->res[DR_RES_TEETH] < c->teeth) return false;
    g->res[DR_RES_FUR] -= c->fur;
    g->res[DR_RES_SCALES] -= c->scales;
    g->res[DR_RES_TEETH] -= c->teeth;
    switch ((dr_trade_t)item) {
        case DR_TRADE_SCALES: g->res[DR_RES_SCALES] += 1; break;
        case DR_TRADE_TEETH:  g->res[DR_RES_TEETH] += 1; break;
        case DR_TRADE_IRON:   g->res[DR_RES_IRON] += 1; break;
        case DR_TRADE_COAL:   g->res[DR_RES_COAL] += 1; break;
        case DR_TRADE_STEEL:  g->res[DR_RES_STEEL] += 1; break;
        case DR_TRADE_BULLETS:g->res[DR_RES_BULLETS] += 1; break;
        case DR_TRADE_MEDICINE: g->res[DR_RES_MEDICINE] += 1; break;
        case DR_TRADE_COMPASS:
            g->flags |= (uint64_t)1u << DR_FLAG_COMPASS;
            break;
        default: return false;
    }
    // 买入计入累计(原版 purchase 也走 stores 增量)
    switch ((dr_trade_t)item) {
        case DR_TRADE_SCALES: g->res_total[DR_RES_SCALES] += 1; break;
        case DR_TRADE_TEETH:  g->res_total[DR_RES_TEETH] += 1; break;
        case DR_TRADE_IRON:   g->res_total[DR_RES_IRON] += 1; break;
        case DR_TRADE_COAL:   g->res_total[DR_RES_COAL] += 1; break;
        case DR_TRADE_STEEL:  g->res_total[DR_RES_STEEL] += 1; break;
        case DR_TRADE_BULLETS:g->res_total[DR_RES_BULLETS] += 1; break;
        case DR_TRADE_MEDICINE: g->res_total[DR_RES_MEDICINE] += 1; break;
        default: break;
    }
    return true;
}

// 陌生人"沉睡→帮忙"(原版 Room.onArrival:玩家回到房间时触发,非自动)
bool dr_rules_builder_visit(dr_game_t *g) {
    if (g->builder_lv != DR_BUILDER_SLEEP) return false;
    g->builder_lv = DR_BUILDER_HELP;
    return true;
}

// ---- 周期心跳 ----
bool dr_rules_tick(dr_rules_rt_t *rt, dr_game_t *g, uint32_t now_ms,
                   dr_rules_event_t *out_ev, uint16_t *out_arg) {
    bool changed = false;
    if (out_ev) *out_ev = DR_RT_EV_NONE;
    if (out_arg) *out_arg = 0;

    // 火焰衰减(原版 coolFire):降档前建造者会先添柴
    if (g->fire_lv > DR_FIRE_DEAD &&
        (int32_t)(now_ms - rt->fire_deadline_ms) >= 0) {
        rt->fire_deadline_ms = now_ms + DR_FIRE_LEVEL_SECONDS * 1000u;
        if (g->fire_lv <= DR_FIRE_FLICKERING &&
            g->builder_lv >= DR_BUILDER_HELP && g->res[DR_RES_WOOD] > 0) {
            g->res[DR_RES_WOOD] -= 1;
            if (g->fire_lv < DR_FIRE_ROARING) g->fire_lv++;
            if (out_ev) *out_ev = DR_RT_EV_BUILDER_STOKE;
        }
        if (g->fire_lv > DR_FIRE_DEAD) {
            g->fire_lv--;
            if (out_ev && *out_ev == DR_RT_EV_NONE)
                *out_ev = (g->fire_lv == DR_FIRE_DEAD) ? DR_RT_EV_FIRE_OUT
                                                       : DR_RT_EV_FIRE_DOWN;
        }
        changed = true;
    }

    // 陌生人入场(原版 onFireChange:火首次 ≥"跳动"且未见过她)
    if (g->builder_lv == DR_BUILDER_NONE && g->fire_lv >= DR_FIRE_FLICKERING) {
        g->builder_lv = DR_BUILDER_DOWN;
        rt->forest_unlock_ms = now_ms + DR_FOREST_UNLOCK_S * 1000u;
        rt->builder_next_ms = now_ms + DR_BUILDER_STATE_SECONDS * 1000u;
        if (out_ev) *out_ev = DR_RT_EV_BUILDER_IN;
        changed = true;
    }

    // 森林解锁(原版 unlockForest:柴火只剩 4,村庄页开启)
    if (rt->forest_unlock_ms != 0 &&
        (int32_t)(now_ms - rt->forest_unlock_ms) >= 0) {
        rt->forest_unlock_ms = 0;
        g->res[DR_RES_WOOD] = 4u;   // 原版直接置 4(不是加)
        g->flags |= (uint64_t)1u << DR_FLAG_FOREST;
        if (out_ev) *out_ev = DR_RT_EV_FOREST;
        changed = true;
    }

    // 室温:每 30s 向火焰档移动一步(原版 adjustTemp)
    if ((int32_t)(now_ms - rt->temp_next_ms) >= 0) {
        rt->temp_next_ms = now_ms + DR_TEMP_LEVEL_SECONDS * 1000u;
        if (g->temp_lv < g->fire_lv) { g->temp_lv++; changed = true; }
        else if (g->temp_lv > g->fire_lv) { g->temp_lv--; changed = true; }
    }

    // 建造者恢复:室温 ≥"暖"后每 30s 推进一态(原版 updateBuilderState)
    // 沉睡→帮忙不走定时器——原版在玩家回到房间时才触发(dr_rules_builder_visit)
    if (g->builder_lv >= DR_BUILDER_DOWN && g->builder_lv < DR_BUILDER_SLEEP &&
        (int32_t)(now_ms - rt->builder_next_ms) >= 0) {
        rt->builder_next_ms = now_ms + DR_BUILDER_STATE_SECONDS * 1000u;
        if (g->temp_lv >= DR_TEMP_WARM) {
            g->builder_lv++;
            if (out_ev) *out_ev = (g->builder_lv == DR_BUILDER_SHIVER)
                                      ? DR_RT_EV_BUILDER_SHIVER
                                      : DR_RT_EV_BUILDER_SLEEP;
            changed = true;
        }
    }

    // 收入:每 10s(原版 income delay)
    if ((int32_t)(now_ms - rt->econ_next_ms) >= 0) {
        rt->econ_next_ms = now_ms + DR_ECONOMY_TICK_S * 1000u;
        dr_income_out_t e;
        income_step(rt, g, &e);
        changed = true;
    }

    // 流浪者:有房有空间则按批到达(原版 increasePopulation)
    if (g->building_lv[DR_BLD_HUT] > 0 &&
        (int32_t)(now_ms - rt->wanderer_next_ms) >= 0) {
        dr_rules_wanderer_schedule(rt, now_ms);
        uint16_t cap = dr_rules_pop_cap(g);
        if (g->population < cap) {
            uint32_t space = cap - g->population;
            uint32_t num = space / 2u +
                           dr_rng_below(&rt->rng, space / 2u + 1u);
            if (num == 0) num = 1;
            if (num > space) num = space;
            g->population += (uint16_t)num;
            if (out_ev) *out_ev = DR_RT_EV_WANDERER;
            if (out_arg) *out_arg = (uint16_t)num;
            changed = true;
        }
    }
    return changed;
}

// ---- 离线结算(设备适配:原版关页无收益;本作按同一收入表逐 tick 补算) ----
uint32_t dr_rules_offline_settle(dr_rules_rt_t *rt, dr_game_t *g,
                                 uint32_t now_ts_s, uint32_t now_ms,
                                 dr_offline_yield_t *y) {
    (void)now_ms;
    dr_offline_yield_t zero = {0, 0, 0, 0, 0, 0, false};
    if (y) *y = zero;
    if (now_ts_s < g->saved_at_ts) {
        g->saved_at_ts = now_ts_s;          // 时钟回拨:锚定当下
        return 0;
    }
    if (now_ts_s == g->saved_at_ts) return 0;
    uint32_t delta_s = now_ts_s - g->saved_at_ts;

    // 火焰:离线即熄(设备口径:计时基准不落盘)
    if (g->fire_lv > DR_FIRE_DEAD) {
        g->fire_lv = DR_FIRE_DEAD;
        if (y) y->fire_out = true;
    }

    // 收入:按等效 tick 逐 tick 结算(8h 封顶,超出 25% 折算)
    uint32_t ticks = dr_offline_ticks(g, now_ts_s);
    if (y) y->ticks = ticks;
    for (uint32_t i = 0; i < ticks; i++) {
        dr_income_out_t e;
        income_step(rt, g, &e);
        if (y) {
            y->wood += e.wood; y->fur += e.fur; y->meat += e.meat;
            y->leather += e.leather; y->food += e.food;
        }
    }

    // 陷阱:每个完整周期按"查看陷阱"同口径补掷(不耗饵——饵只在手动查看时消耗)
    if (g->building_lv[DR_BLD_TRAP] > 0) {
        uint32_t cap_s = delta_s > DR_OFFLINE_CAP_S ? DR_OFFLINE_CAP_S : delta_s;
        uint32_t periods = cap_s / DR_TRAP_PERIOD_S;
        uint32_t rolls = g->building_lv[DR_BLD_TRAP];
        for (uint32_t i = 0; i < periods; i++) {
            for (uint32_t r = 0; r < rolls; r++)
                trap_drop(g, dr_rng_below(&rt->rng, 1000u));
        }
    }
    g->rng_seed_state = rt->rng.s;   // 离线推进的序列写回档
    return ticks;
}
