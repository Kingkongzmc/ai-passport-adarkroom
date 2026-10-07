// main/game_darkroom/dr_events.c —— 事件运行时:条件求值、事件挑选、效果执行。
#include <string.h>

#include "dr_events.h"
#include "dr_util.h"

// 减员裁岗:在编总数不得超过人口,否则幽灵工人照常产出(§3.3 硬化)。
// 从职业表尾端撤起(确定性,便于测试;尾端未用槽位恒 0 不影响结果)。
static void clip_jobs(dr_game_t *g) {
    const int jobs = (int)(sizeof(g->job) / sizeof(g->job[0]));
    uint32_t assigned = 0;
    for (int i = 0; i < jobs; i++) assigned += g->job[i];
    for (int i = jobs - 1; i >= 0 && assigned > g->population; i--) {
        uint32_t cut = assigned - g->population;
        if (g->job[i] > cut) { g->job[i] -= (uint16_t)cut; assigned -= cut; }
        else                { assigned -= g->job[i]; g->job[i] = 0; }
    }
}

// 单条件求值。纯函数,不依赖 RNG 以外的状态。
static bool cond_eval(const dr_cond_t *c, const dr_game_t *g, dr_rng_t *rng) {
    switch ((dr_cond_op_t)c->op) {
        case DR_COND_RES_GE:
            return g->res[c->arg1] >= c->arg2;
        case DR_COND_RES_LT:
            return g->res[c->arg1] < c->arg2;
        case DR_COND_BUILDING_GE:
            return g->building_lv[c->arg1] >= c->arg2;
        case DR_COND_BUILDING_LT:
            return g->building_lv[c->arg1] < c->arg2;
        case DR_COND_FLAG_SET:
            return (g->flags >> (c->arg1 & 63u)) & 1u;
        case DR_COND_FLAG_CLEAR:
            return !((g->flags >> (c->arg1 & 63u)) & 1u);
        case DR_COND_EVENT_SEEN:
            return g->event_count[c->arg1] > 0;
        case DR_COND_CHANCE:
            return dr_rng_chance(rng, c->arg2);
        case DR_COND_POP_GE:
            return g->population >= c->arg2;
        case DR_COND_POP_LT:
            return g->population < c->arg2;
        case DR_COND_NONE:
            return true;
        default:
            return false;  // 未知操作码视为不满足,坏数据不触发事件
    }
}

static bool conds_all(const dr_event_t *e, const dr_game_t *g, dr_rng_t *rng) {
    for (int i = 0; i < DR_EVENT_MAX_CONDS; i++) {
        if (e->conds[i].op == DR_COND_NONE) break;
        if (!cond_eval(&e->conds[i], g, rng)) return false;
    }
    return true;
}

void dr_event_session_init(dr_event_session_t *s) {
    memset(s, 0, sizeof(*s));
}

dr_event_result_t dr_event_pick(const dr_event_t *events, uint16_t count,
                                const dr_game_t *g, dr_event_session_t *sess,
                                uint32_t now_ts, uint8_t scene,
                                dr_rng_t *rng, uint16_t *out_idx) {
    // 全局节拍门(原版 _EVENT_TIME_RANGE 3~6 分钟):上次触发后未到时
    // 任何事件都不轮询,避免每事件独立冷却导致的整体频率失控。
    if (sess->next_ok_ts != 0 && now_ts < sess->next_ok_ts)
        return DR_EVENT_NONE;
    int best = -1;
    for (uint16_t i = 0; i < count; i++) {
        const dr_event_t *e = &events[i];
        if (!(e->scene_mask & scene)) continue;
        if (e->max_seen != 0 && g->event_count[i] >= e->max_seen) continue;
        // 会话内冷却:同一事件两次触发至少间隔 cooldown_s(离线时段不推进冷却,
        // 冷却起点为本会话内上次触发;新会话开始即视为冷却已过)。
        if (e->cooldown_s != 0 && sess->last_fire_ts[i] != 0 &&
            now_ts < sess->last_fire_ts[i] + e->cooldown_s) continue;
        if (!conds_all(e, g, rng)) continue;
        if (best < 0 ||
            e->priority > events[best].priority) {
            best = (int)i;  // 同优先级取更小 id(遍历序保证)
        }
    }
    if (best < 0) return DR_EVENT_NONE;
    // 掷定下一次的全局节拍:180+rand(180) 秒(原版 3~6 分钟)
    sess->next_ok_ts = now_ts + 180u + dr_rng_below(rng, 181u);
    if (out_idx) *out_idx = (uint16_t)best;
    return DR_EVENT_FIRED;
}

dr_event_result_t dr_event_poll(const dr_event_t *events, uint16_t count,
                                dr_game_t *g, dr_event_session_t *sess,
                                uint32_t now_ts, uint8_t scene,
                                uint16_t *out_idx) {
    dr_rng_t rng;
    dr_rng_seed(&rng, g->rng_seed_state ^ 0xC0FFEEu);
    dr_event_result_t r = dr_event_pick(events, count, g, sess, now_ts,
                                        scene, &rng, out_idx);
    // 未触发也回写推进后的序列:否则 CHANCE 未命中时下次轮询重新播种,
    // 永远重掷出同一个值(§3.3 硬化)。
    g->rng_seed_state = rng.s;
    return r;
}

// 选项代价是否付得起(UI 灰显/跳过焦点用;choose 内还会再核一次)。
// avail_flag_clear:要求旗标未置位(原版按钮 available 的"已有则隐藏",
// 设备形态取灰显等价);旗标代价(DR_COST_FLAG)要求已持有。
bool dr_event_choice_affordable(const dr_event_t *e, uint8_t choice_idx,
                                const dr_game_t *g) {
    if (e == NULL || choice_idx >= e->choice_count) return false;
    const dr_event_choice_t *ch = &e->choices[choice_idx];
    if (ch->avail_flag_clear != DR_COST_NONE &&
        ((g->flags >> (ch->avail_flag_clear & 63u)) & 1u))
        return false;
    for (int i = 0; i < 3; i++) {
        uint8_t r = ch->cost_res[i];
        if (r == DR_COST_NONE) continue;
        if (r == DR_COST_FLAG) {
            if (!((g->flags >> (ch->cost_amt[i] & 63u)) & 1u)) return false;
            continue;
        }
        if (r >= DR_RES_KIND_COUNT) return false;
        if (g->res[r] < ch->cost_amt[i]) return false;
    }
    return true;
}

static void eff_apply(const dr_eff_t *f, dr_game_t *g, dr_rng_t *rng,
                      int16_t *chain_out) {
    switch ((dr_eff_op_t)f->op) {
        case DR_EFF_RES_ADD: {
            uint32_t *cur = &g->res[f->arg1];
            if (f->arg2 >= 0) {
                *cur += (uint32_t)f->arg2;
                g->res_total[f->arg1] += (uint32_t)f->arg2;  // 累计口径
            } else {
                uint32_t sub = (uint32_t)(-(int32_t)f->arg2);
                *cur = (*cur > sub) ? (*cur - sub) : 0;      // 存量钳到 0
            }
            break;
        }
        case DR_EFF_FLAG_SET:
            g->flags |= (uint64_t)1u << (f->arg1 & 63u);
            break;
        case DR_EFF_FLAG_CLEAR:
            g->flags &= ~((uint64_t)1u << (f->arg1 & 63u));
            break;
        case DR_EFF_GOTO_EVENT:
            if (*chain_out < 0) *chain_out = (int16_t)f->arg1;  // 先到先得
            break;
        case DR_EFF_GOTO_W:
            // 加权分支:命中即定链;未中继续后续效果(可再挂 GOTO_W/GOTO 兜底)
            if (*chain_out < 0 && dr_rng_chance(rng, (uint16_t)f->arg2))
                *chain_out = (int16_t)f->arg1;
            break;
        case DR_EFF_END_ACT:
            g->flags |= (uint64_t)1u << (f->arg1 & 63u);  // 幕标记也走 flags
            break;
        case DR_EFF_POP_ADD: {
            int32_t p = (int32_t)g->population + f->arg2;
            g->population = (p < 0) ? 0 : (uint16_t)p;
            clip_jobs(g);
            break;
        }
        case DR_EFF_POP_FRAC: {
            // 损失 floor(pop×permille/1000),至少 1(原版随机减员的中值离散化)
            uint32_t loss = ((uint32_t)g->population * (uint32_t)f->arg2) / 1000u;
            if (loss == 0) loss = 1;
            if (loss > g->population) loss = g->population;
            g->population = (uint16_t)(g->population - loss);
            clip_jobs(g);
            break;
        }
        case DR_EFF_BLD_ADD: {
            int32_t lv = (int32_t)g->building_lv[f->arg1] + f->arg2;
            g->building_lv[f->arg1] = (lv < 0) ? 0 : (uint8_t)lv;
            break;
        }
        case DR_EFF_BLD_FRAC: {
            uint8_t lv = g->building_lv[f->arg1];
            uint32_t loss = ((uint32_t)lv * (uint32_t)f->arg2) / 1000u;
            if (loss == 0) loss = 1;
            if (loss > lv) loss = lv;
            g->building_lv[f->arg1] = (uint8_t)(lv - loss);
            break;
        }
        case DR_EFF_THIEF_HANG:
            // 吊死窃贼(原版 global.js hang:thieves=2、停收入、追回 stolen)
            g->thieves_lv = 2;
            g->flags &= ~((uint64_t)1u << DR_FLAG_THIEF_ACTIVE);
            g->res[DR_RES_WOOD] += g->stolen_wood;
            g->res_total[DR_RES_WOOD] += g->stolen_wood;
            g->res[DR_RES_FUR] += g->stolen_fur;
            g->res_total[DR_RES_FUR] += g->stolen_fur;
            g->res[DR_RES_MEAT] += g->stolen_meat;
            g->res_total[DR_RES_MEAT] += g->stolen_meat;
            g->stolen_wood = g->stolen_fur = g->stolen_meat = 0;
            break;
        case DR_EFF_THIEF_SPARE:
            // 放过窃贼(原版 spare:thieves=2、停收入、addPerk stealthy)
            g->thieves_lv = 2;
            g->flags &= ~((uint64_t)1u << DR_FLAG_THIEF_ACTIVE);
            g->flags |= (uint64_t)1u << DR_FLAG_PERK_STEALTHY;
            break;
        case DR_EFF_NONE:
            break;
    }
}

int16_t dr_event_choose(const dr_event_t *events, uint16_t count,
                        dr_game_t *g, dr_event_session_t *sess,
                        uint32_t now_ts, dr_rng_t *rng,
                        uint16_t event_idx, uint8_t choice_idx) {
    (void)count;
    const dr_event_t *e = &events[event_idx];
    if (choice_idx >= e->choice_count) return -1;
    if (!dr_event_choice_affordable(e, choice_idx, g))
        return DR_EVENT_CHOOSE_REJECTED;

    // 代价先扣(不足已在上面拦下);旗标代价=清除(消耗品,如火把)
    for (int i = 0; i < 3; i++) {
        uint8_t r = e->choices[choice_idx].cost_res[i];
        if (r == DR_COST_NONE) continue;
        if (r == DR_COST_FLAG) {
            g->flags &= ~((uint64_t)1u << (e->choices[choice_idx].cost_amt[i] & 63u));
            continue;
        }
        g->res[r] -= e->choices[choice_idx].cost_amt[i];
    }

    int16_t chain = -1;
    const dr_eff_t *effs = e->choices[choice_idx].effects;
    for (int i = 0; i < 4; i++) {
        if (effs[i].op == DR_EFF_NONE) break;
        eff_apply(&effs[i], g, rng, &chain);
    }
    g->event_count[event_idx]++;
    sess->last_fire_ts[event_idx] = now_ts;
    return chain;
}
