// main/game_darkroom/dr_events.c —— 事件运行时:条件求值、事件挑选、效果执行。
#include <string.h>

#include "dr_events.h"
#include "dr_util.h"

// 单条件求值。纯函数,不依赖 RNG 以外的状态。
static bool cond_eval(const dr_cond_t *c, const dr_game_t *g, dr_rng_t *rng) {
    switch ((dr_cond_op_t)c->op) {
        case DR_COND_RES_GE:
            return g->res[c->arg1] >= c->arg2;
        case DR_COND_RES_LT:
            return g->res[c->arg1] < c->arg2;
        case DR_COND_BUILDING_GE:
            return g->building_lv[c->arg1] >= c->arg2;
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
    if (out_idx) *out_idx = (uint16_t)best;
    return DR_EVENT_FIRED;
}

static void eff_apply(const dr_eff_t *f, dr_game_t *g, int16_t *chain_out) {
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
            *chain_out = (int16_t)f->arg1;
            break;
        case DR_EFF_END_ACT:
            g->flags |= (uint64_t)1u << (f->arg1 & 63u);  // 幕标记也走 flags
            break;
        case DR_EFF_POP_ADD: {
            int32_t p = (int32_t)g->population + f->arg2;
            g->population = (p < 0) ? 0 : (uint16_t)p;
            break;
        }
        case DR_EFF_NONE:
            break;
    }
}

int16_t dr_event_choose(const dr_event_t *events, uint16_t count,
                        dr_game_t *g, dr_event_session_t *sess,
                        uint32_t now_ts, uint16_t event_idx, uint8_t choice_idx) {
    (void)count;
    const dr_event_t *e = &events[event_idx];
    if (choice_idx >= e->choice_count) return -1;

    int16_t chain = -1;
    const dr_eff_t *effs = e->choices[choice_idx].effects;
    for (int i = 0; i < 2; i++) {
        if (effs[i].op == DR_EFF_NONE) break;
        eff_apply(&effs[i], g, &chain);
    }
    g->event_count[event_idx]++;
    sess->last_fire_ts[event_idx] = now_ts;
    return chain;
}
