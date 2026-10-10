// main/game_darkroom/dr_events_data.c —— 事件表(M6 灌装,切片一+切片二)。
// 纯数据,无逻辑;文案引用 dr_text.h 的 id。
// v3 起"陌生人"不再走事件对话框——对齐原版:陌生人(建造者)是规则层状态机
// (晕倒→发抖→沉睡→帮忙,室温驱动,见 dr_rules.c),无玩家选择分支。
// 取证:原版 script/events/room.js + events/outside.js + events/global.js
// (2026-10-07 jsdelivr),条件/概率/数值逐条对齐;离散化适配(减员数取中值、
// 兑换改固定量、地图揭示确定性化)注明在 IMPLEMENTATION_STATUS M6 行。
// 原版模块对应:Room→DR_SCENE_HOME,Outside→DR_SCENE_VILLAGE,Global→HOME|VILLAGE。
// 切片二:军队突袭(cityCleared 门)、游牧商人(5 选项/3 代价槽/自链常驻)、
// 窃贼(规则层触发链+THIEF 效果码)、斥候/宗师(Perk 体系;
// 斥候买地图的揭示由 UI 桥接 dr_world_apply_map——迷雾是会话态)。
#include "dr_events.h"
#include "dr_rules.h"   // DR_BLD_HUT/DR_BLD_TRAP 等建筑 id(条件引用)

#define C(op, a1, a2) { (uint8_t)(op), (a1), (a2) }
#define E(op, a1, a2) { (uint8_t)(op), (a1), (int16_t)(a2) }
#define COND_NONE C(DR_COND_NONE, 0, 0)
#define EFF_NONE  E(DR_EFF_NONE, 0, 0)

// 无代价选项(e0..e3 按需填 EFF_NONE;效果参数不加括号——E 宏展开为
// 裸花括号组,包一层括号会被 GCC 当语句表达式)
#define FREE_CH(txt, e0, e1, e2, e3)                                  \
    { .text_id = (txt), .avail_flag_clear = DR_COST_NONE,             \
      .cost_res = { DR_COST_NONE, DR_COST_NONE, DR_COST_NONE },       \
      .cost_amt = { 0, 0, 0 },                                        \
      .effects = { e0, e1, e2, e3 } }

// 单资源代价选项
#define COST_CH(txt, r1, a1, e0, e1, e2, e3)                          \
    { .text_id = (txt), .avail_flag_clear = DR_COST_NONE,             \
      .cost_res = { (uint8_t)(r1), DR_COST_NONE, DR_COST_NONE },      \
      .cost_amt = { (a1), 0, 0 },                                     \
      .effects = { e0, e1, e2, e3 } }

// 双资源代价选项
#define COST2_CH(txt, r1, a1, r2, a2, e0, e1, e2, e3)                 \
    { .text_id = (txt), .avail_flag_clear = DR_COST_NONE,             \
      .cost_res = { (uint8_t)(r1), (uint8_t)(r2), DR_COST_NONE },     \
      .cost_amt = { (a1), (a2), 0 },                                  \
      .effects = { e0, e1, e2, e3 } }

// 三资源代价选项(M6 切片二:游牧商人罗盘 / 斥候学侦查)
#define COST3_CH(txt, r1, a1, r2, a2, r3, a3, e0, e1, e2, e3)         \
    { .text_id = (txt), .avail_flag_clear = DR_COST_NONE,             \
      .cost_res = { (uint8_t)(r1), (uint8_t)(r2), (uint8_t)(r3) },    \
      .cost_amt = { (a1), (a2), (a3) },                               \
      .effects = { e0, e1, e2, e3 } }

// 表项下标(链式跳转引用;变更顺序须同步)
enum {
    DR_EV_NOISE_O = 0, DR_EV_NOISE_O_STUFF, DR_EV_NOISE_O_NOTHING,
    DR_EV_NOISE_I, DR_EV_NOISE_I_S, DR_EV_NOISE_I_T, DR_EV_NOISE_I_C,
    DR_EV_BEGGAR, DR_EV_BEGGAR_S, DR_EV_BEGGAR_T, DR_EV_BEGGAR_C,
    DR_EV_SHADY, DR_EV_SHADY_BUILT, DR_EV_SHADY_STOLEN,
    DR_EV_WAND_W, DR_EV_WAND_W_BACK_S, DR_EV_WAND_W_BACK_B, DR_EV_WAND_GONE,
    DR_EV_WAND_F, DR_EV_WAND_F_BACK_S, DR_EV_WAND_F_BACK_B,
    DR_EV_SICKMAN, DR_EV_SICKMAN_ALLOY, DR_EV_SICKMAN_SC, DR_EV_SICKMAN_NONE,
    DR_EV_TRAPS, DR_EV_TRAPS_CATCH, DR_EV_TRAPS_LOST,
    DR_EV_FIRE,
    DR_EV_SICK, DR_EV_SICK_HEALED, DR_EV_SICK_DEATH,
    DR_EV_PLAGUE, DR_EV_PLAGUE_KEPT, DR_EV_PLAGUE_RAGE,
    DR_EV_BEAST,
    // ---- 切片二 ----
    DR_EV_RAID,
    DR_EV_NOMAD,
    DR_EV_THIEF, DR_EV_THIEF_HANGED, DR_EV_THIEF_SPARED,
    DR_EV_SCOUT,
    DR_EV_MASTER, DR_EV_MASTER_AGREE,
    // ---- M5(只许尾部追加:event_count 按下标落盘,中间插入会错位存档) ----
    DR_EV_SICKMAN_CELL,     // 病人 cells 分支(能量元件 v6 实装后还原原版权重)
    DR_EV_EXEC, DR_EV_EXEC_PEACE,   // 执行者(星舰首访;scene 0 不轮询,UI 触发)
};

static const dr_event_t k_events[] = {
    // ---- 墙外动静(room.js Noises Outside:30% 得 100 木+10 毛) ----
    [DR_EV_NOISE_O] = {
        .text_id = DR_T_EV_NOISE_O, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_RES_GE, DR_RES_WOOD, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_NOISE_O_C0,
                E(DR_EFF_GOTO_W, DR_EV_NOISE_O_STUFF, 300),
                E(DR_EFF_GOTO_EVENT, DR_EV_NOISE_O_NOTHING, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_IGNORE, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_NOISE_O_STUFF] = {
        .text_id = DR_T_EV_NOISE_O_STUFF, .scene_mask = 0,   // 仅链式可达,不轮询
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, 100),
                E(DR_EFF_RES_ADD, DR_RES_FUR, 10),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_NOISE_O_NOTHING] = {
        .text_id = DR_T_EV_NOISE_O_NOTHING, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 库房异响(room.js Noises Inside:原版按木量 10% 折损 5:1 兑换;
    //      离散化为固定 -50 木 → +10 鳞/牙/布,50%/30%/20%) ----
    [DR_EV_NOISE_I] = {
        .text_id = DR_T_EV_NOISE_I, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_RES_GE, DR_RES_WOOD, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_NOISE_I_C0,
                E(DR_EFF_GOTO_W, DR_EV_NOISE_I_S, 500),
                E(DR_EFF_GOTO_W, DR_EV_NOISE_I_T, 600),
                E(DR_EFF_GOTO_EVENT, DR_EV_NOISE_I_C, 0),
                EFF_NONE),
            FREE_CH(DR_T_EV_IGNORE, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_NOISE_I_S] = {
        .text_id = DR_T_EV_NOISE_I_S, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, -50),
                E(DR_EFF_RES_ADD, DR_RES_SCALES, 10),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_NOISE_I_T] = {
        .text_id = DR_T_EV_NOISE_I_T, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, -50),
                E(DR_EFF_RES_ADD, DR_RES_TEETH, 10),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_NOISE_I_C] = {
        .text_id = DR_T_EV_NOISE_I_C, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, -50),
                E(DR_EFF_RES_ADD, DR_RES_CLOTH, 10),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 乞讨者(room.js The Beggar:50/100 毛 → 20 鳞/牙/布;
    //      给 50 → 鳞50/牙30/布20;给 100 → 牙50/鳞30/布20,同原版权重互换) ----
    [DR_EV_BEGGAR] = {
        .text_id = DR_T_EV_BEGGAR, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_RES_GE, DR_RES_FUR, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_GIVE_FUR, DR_RES_FUR, 50,
                E(DR_EFF_GOTO_W, DR_EV_BEGGAR_S, 500),
                E(DR_EFF_GOTO_W, DR_EV_BEGGAR_T, 600),
                E(DR_EFF_GOTO_EVENT, DR_EV_BEGGAR_C, 0),
                EFF_NONE),
            COST_CH(DR_T_EV_GIVE_MORE_FUR, DR_RES_FUR, 100,
                E(DR_EFF_GOTO_W, DR_EV_BEGGAR_T, 500),
                E(DR_EFF_GOTO_W, DR_EV_BEGGAR_S, 600),
                E(DR_EFF_GOTO_EVENT, DR_EV_BEGGAR_C, 0),
                EFF_NONE),
            FREE_CH(DR_T_EV_SEND_AWAY, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 3,
    },
    [DR_EV_BEGGAR_S] = {
        .text_id = DR_T_EV_BEGGAR_S, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_SCALES, 20),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_BEGGAR_T] = {
        .text_id = DR_T_EV_BEGGAR_T, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_TEETH, 20),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_BEGGAR_C] = {
        .text_id = DR_T_EV_BEGGAR_C, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_CLOTH, 20),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 阴险的工匠(room.js The Shady Builder:hut 5..19 时,300 木 60% 得一屋) ----
    [DR_EV_SHADY] = {
        .text_id = DR_T_EV_SHADY, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_BUILDING_GE, DR_BLD_HUT, 5),
                   C(DR_COND_BUILDING_LT, DR_BLD_HUT, 20),
                   COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_SHADY_C0, DR_RES_WOOD, 300,
                E(DR_EFF_GOTO_W, DR_EV_SHADY_BUILT, 600),
                E(DR_EFF_GOTO_EVENT, DR_EV_SHADY_STOLEN, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_SHADY_C1, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_SHADY_BUILT] = {
        .text_id = DR_T_EV_SHADY_BUILT, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_BLD_ADD, DR_BLD_HUT, 1),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_SHADY_STOLEN] = {
        .text_id = DR_T_EV_SHADY_STOLEN, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 神秘的流浪者·木(room.js:给 100→50% 回 300;给 500→30% 回 1500。
    //      原版 60 秒后延迟返回,离散化为选择即结算) ----
    [DR_EV_WAND_W] = {
        .text_id = DR_T_EV_WAND_W, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_RES_GE, DR_RES_WOOD, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_GIVE_WOOD, DR_RES_WOOD, 100,
                E(DR_EFF_GOTO_W, DR_EV_WAND_W_BACK_S, 500),
                E(DR_EFF_GOTO_EVENT, DR_EV_WAND_GONE, 0),
                EFF_NONE, EFF_NONE),
            COST_CH(DR_T_EV_GIVE_MORE_WOOD, DR_RES_WOOD, 500,
                E(DR_EFF_GOTO_W, DR_EV_WAND_W_BACK_B, 300),
                E(DR_EFF_GOTO_EVENT, DR_EV_WAND_GONE, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_SEND_AWAY, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 3,
    },
    [DR_EV_WAND_W_BACK_S] = {
        .text_id = DR_T_EV_WAND_BACK_W, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, 300),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_WAND_W_BACK_B] = {
        .text_id = DR_T_EV_WAND_BACK_W, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_WOOD, 1500),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 神秘的流浪者·毛(同构;给毛版) ----
    [DR_EV_WAND_F] = {
        .text_id = DR_T_EV_WAND_F, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_RES_GE, DR_RES_FUR, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_GIVE_FUR, DR_RES_FUR, 100,
                E(DR_EFF_GOTO_W, DR_EV_WAND_F_BACK_S, 500),
                E(DR_EFF_GOTO_EVENT, DR_EV_WAND_GONE, 0),
                EFF_NONE, EFF_NONE),
            COST_CH(DR_T_EV_GIVE_MORE_FUR, DR_RES_FUR, 500,
                E(DR_EFF_GOTO_W, DR_EV_WAND_F_BACK_B, 300),
                E(DR_EFF_GOTO_EVENT, DR_EV_WAND_GONE, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_SEND_AWAY, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 3,
    },
    [DR_EV_WAND_F_BACK_S] = {
        .text_id = DR_T_EV_WAND_BACK_F, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_FUR, 300),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_WAND_F_BACK_B] = {
        .text_id = DR_T_EV_WAND_BACK_F, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_FUR, 1500),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_WAND_GONE] = {
        .text_id = DR_T_EV_WAND_GONE, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 病人(room.js The Sick Man:1 药 → 10% 异金/30% 能量元件×3/50% 鳞×5;
    //      v6 能量元件实装,还原原版四分支权重 100/300/500/落空) ----
    [DR_EV_SICKMAN] = {
        .text_id = DR_T_EV_SICKMAN, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_RES_GE, DR_RES_MEDICINE, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_GIVE_MED, DR_RES_MEDICINE, 1,
                E(DR_EFF_GOTO_W, DR_EV_SICKMAN_ALLOY, 100),
                E(DR_EFF_GOTO_W, DR_EV_SICKMAN_CELL, 300),
                E(DR_EFF_GOTO_W, DR_EV_SICKMAN_SC, 500),
                E(DR_EFF_GOTO_EVENT, DR_EV_SICKMAN_NONE, 0)),
            FREE_CH(DR_T_EV_SEND_AWAY, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_SICKMAN_ALLOY] = {
        .text_id = DR_T_EV_SICKMAN_ALLOY, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_ALIEN, 1),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_SICKMAN_SC] = {
        .text_id = DR_T_EV_SICKMAN_SC, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_SCALES, 5),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_SICKMAN_CELL] = {
        .text_id = DR_T_EV_SICKMAN_CELL, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_ENERGY_CELL, 3),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_SICKMAN_NONE] = {
        .text_id = DR_T_EV_SICKMAN_NOTHING, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 毁坏的陷阱(outside.js A Ruined Trap:陷阱损半(原版 1..全数随机,
    //      离散化取半);追踪 50% 得 毛100/肉100/牙10) ----
    [DR_EV_TRAPS] = {
        .text_id = DR_T_EV_TRAPS, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_BUILDING_GE, DR_BLD_TRAP, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_TRAPS_C0,
                E(DR_EFF_BLD_FRAC, DR_BLD_TRAP, 500),
                E(DR_EFF_GOTO_W, DR_EV_TRAPS_CATCH, 500),
                E(DR_EFF_GOTO_EVENT, DR_EV_TRAPS_LOST, 0),
                EFF_NONE),
            FREE_CH(DR_T_EV_TRAPS_C1,
                E(DR_EFF_BLD_FRAC, DR_BLD_TRAP, 500),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_TRAPS_CATCH] = {
        .text_id = DR_T_EV_TRAPS_CATCH, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_RES_ADD, DR_RES_FUR, 100),
                E(DR_EFF_RES_ADD, DR_RES_MEAT, 100),
                E(DR_EFF_RES_ADD, DR_RES_TEETH, 10),
                EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_TRAPS_LOST] = {
        .text_id = DR_T_EV_TRAPS_LOST, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 小屋失火(outside.js Fire:hut≥1 且 pop>50;毁一屋,
    //      屋内 4 人罹难(原版按满/半员屋加权,离散化取满员 4)) ----
    [DR_EV_FIRE] = {
        .text_id = DR_T_EV_FIRE, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_BUILDING_GE, DR_BLD_HUT, 1),
                   C(DR_COND_POP_GE, 0, 51),
                   COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_FIRE_C0,
                E(DR_EFF_BLD_ADD, DR_BLD_HUT, -1),
                E(DR_EFF_POP_ADD, 0, -4),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 疫病(outside.js Sickness:11≤pop<50 且有药;1 药治愈,
    //      放任减员 pop/4(原版 1..pop/2 随机,取中值)) ----
    [DR_EV_SICK] = {
        .text_id = DR_T_EV_SICK, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_POP_GE, 0, 11),
                   C(DR_COND_POP_LT, 0, 50),
                   C(DR_COND_RES_GE, DR_RES_MEDICINE, 1),
                   COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_USE_MED, DR_RES_MEDICINE, 1,
                E(DR_EFF_GOTO_EVENT, DR_EV_SICK_HEALED, 0),
                EFF_NONE, EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_LET_GO,
                E(DR_EFF_GOTO_EVENT, DR_EV_SICK_DEATH, 0),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_SICK_HEALED] = {
        .text_id = DR_T_EV_SICK_HEALED, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_SICK_DEATH] = {
        .text_id = DR_T_EV_SICK_DEATH, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_POP_FRAC, 0, 250),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 大瘟疫(outside.js Plague:pop>50 且有药;买药(鳞70+牙50)/
    //      5 药压制(仍死 4 人,原版 2..6 取中)/放任减员半数(原版 10..89 取中)) ----
    [DR_EV_PLAGUE] = {
        .text_id = DR_T_EV_PLAGUE, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_POP_GE, 0, 51),
                   C(DR_COND_RES_GE, DR_RES_MEDICINE, 1),
                   COND_NONE, COND_NONE },
        .choices = {
            COST2_CH(DR_T_EV_BUY_MED, DR_RES_SCALES, 70, DR_RES_TEETH, 50,
                E(DR_EFF_RES_ADD, DR_RES_MEDICINE, 1),
                EFF_NONE, EFF_NONE, EFF_NONE),
            COST_CH(DR_T_EV_USE_MED, DR_RES_MEDICINE, 5,
                E(DR_EFF_GOTO_EVENT, DR_EV_PLAGUE_KEPT, 0),
                EFF_NONE, EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_LET_GO,
                E(DR_EFF_GOTO_EVENT, DR_EV_PLAGUE_RAGE, 0),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 3,
    },
    [DR_EV_PLAGUE_KEPT] = {
        .text_id = DR_T_EV_PLAGUE_KEPT, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_POP_ADD, 0, -4),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_PLAGUE_RAGE] = {
        .text_id = DR_T_EV_PLAGUE_RAGE, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK,
                E(DR_EFF_POP_FRAC, 0, 500),
                EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 野兽袭击(outside.js A Beast Attack:减员 5(原版 1..10 取中),
    //      得 毛100/肉100/牙10) ----
    [DR_EV_BEAST] = {
        .text_id = DR_T_EV_BEAST, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_POP_GE, 0, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_BEAST_C0,
                E(DR_EFF_POP_ADD, 0, -5),
                E(DR_EFF_RES_ADD, DR_RES_FUR, 100),
                E(DR_EFF_RES_ADD, DR_RES_MEAT, 100),
                E(DR_EFF_RES_ADD, DR_RES_TEETH, 10)),
        },
        .choice_count = 1,
    },

    // ==================== 切片二 ====================

    // ---- 军队突袭(outside.js A Military Raid:pop>0 且城市清剿完成;
    //      减员 20(原版 1..40 随机,取中值),得 子弹10/干肉50) ----
    [DR_EV_RAID] = {
        .text_id = DR_T_EV_RAID, .scene_mask = DR_SCENE_VILLAGE,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_POP_GE, 0, 1),
                   C(DR_COND_FLAG_SET, DR_FLAG_CITY_CLEARED, 0),
                   COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_BEAST_C0,
                E(DR_EFF_POP_ADD, 0, -20),
                E(DR_EFF_RES_ADD, DR_RES_BULLETS, 10),
                E(DR_EFF_RES_ADD, DR_RES_FOOD, 50),
                EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 游牧商人(room.js The Nomad:有毛皮即可遇;鳞100毛/牙200毛/
    //      饵5毛/罗盘300毛+15鳞+5牙(已有则灰)/告别。原版买完商店不关,
    //      引擎对话框选择后即结束——GOTO 链回自身=商店常驻的等效形态;
    //      罗盘与贸易站同旗标,两处任购其一即另一处作废) ----
    [DR_EV_NOMAD] = {
        .text_id = DR_T_EV_NOMAD, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 300,
        .conds = { C(DR_COND_RES_GE, DR_RES_FUR, 1), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_NOMAD_SCALES, DR_RES_FUR, 100,
                E(DR_EFF_RES_ADD, DR_RES_SCALES, 1),
                E(DR_EFF_GOTO_EVENT, DR_EV_NOMAD, 0),
                EFF_NONE, EFF_NONE),
            COST_CH(DR_T_EV_NOMAD_TEETH, DR_RES_FUR, 200,
                E(DR_EFF_RES_ADD, DR_RES_TEETH, 1),
                E(DR_EFF_GOTO_EVENT, DR_EV_NOMAD, 0),
                EFF_NONE, EFF_NONE),
            COST_CH(DR_T_EV_NOMAD_BAIT, DR_RES_FUR, 5,
                E(DR_EFF_RES_ADD, DR_RES_BAIT, 1),
                E(DR_EFF_GOTO_EVENT, DR_EV_NOMAD, 0),
                EFF_NONE, EFF_NONE),
            { .text_id = DR_T_EV_NOMAD_COMPASS,
              .avail_flag_clear = DR_FLAG_COMPASS,
              .cost_res = { DR_RES_FUR, DR_RES_SCALES, DR_RES_TEETH },
              .cost_amt = { 300, 15, 5 },
              .effects = { E(DR_EFF_FLAG_SET, DR_FLAG_COMPASS, 0),
                           E(DR_EFF_GOTO_EVENT, DR_EV_NOMAD, 0),
                           EFF_NONE, EFF_NONE } },
            FREE_CH(DR_T_EV_GOODBYE, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 5,
    },

    // ---- 窃贼(global.js The Thief + state_manager startThieves:
    //      规则层触发(任意储量>5000+罗盘,见 dr_rules.c),thieves=1 时可遇
    //      (小屋/村庄);吊死=追回全部被偷物资,放过=得潜行 Perk,两者都
    //      停止偷窃。max_seen=1:解决即终局 ----
    [DR_EV_THIEF] = {
        .text_id = DR_T_EV_THIEF,
        .scene_mask = DR_SCENE_HOME | DR_SCENE_VILLAGE,
        .priority = 10, .max_seen = 1, .cooldown_s = 0,
        .conds = { C(DR_COND_FLAG_SET, DR_FLAG_THIEF_ACTIVE, 0),
                   COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_EV_THIEF_HANG,
                E(DR_EFF_THIEF_HANG, 0, 0),
                E(DR_EFF_GOTO_EVENT, DR_EV_THIEF_HANGED, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_THIEF_SPARE,
                E(DR_EFF_THIEF_SPARE, 0, 0),
                E(DR_EFF_GOTO_EVENT, DR_EV_THIEF_SPARED, 0),
                EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_THIEF_HANGED] = {
        .text_id = DR_T_EV_THIEF_HANGED, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
    [DR_EV_THIEF_SPARED] = {
        .text_id = DR_T_EV_THIEF_SPARED, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },

    // ---- 斥候(room.js The Scout:世界已解锁(罗盘)时可遇。买地图
    //      200毛+10鳞——揭示由 UI 桥接 dr_world_apply_map(迷雾是会话态,
    //      不在存档引擎辖域;无可揭时 UI 侧灰显不扣钱);学侦查 1000毛+
    //      50鳞+20牙 → 斥候 Perk(揭示半径翻倍+开机确定性揭示一片) ----
    [DR_EV_SCOUT] = {
        .text_id = DR_T_EV_SCOUT, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_FLAG_SET, DR_FLAG_COMPASS, 0),
                   COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST2_CH(DR_T_EV_SCOUT_MAP, DR_RES_FUR, 200, DR_RES_SCALES, 10,
                EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
            { .text_id = DR_T_EV_SCOUT_LEARN,
              .avail_flag_clear = DR_FLAG_PERK_SCOUT,
              .cost_res = { DR_RES_FUR, DR_RES_SCALES, DR_RES_TEETH },
              .cost_amt = { 1000, 50, 20 },
              .effects = { E(DR_EFF_FLAG_SET, DR_FLAG_PERK_SCOUT, 0),
                           EFF_NONE, EFF_NONE, EFF_NONE } },
            FREE_CH(DR_T_EV_GOODBYE, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 3,
    },

    // ---- 宗师(room.js The Wandering Master:世界已解锁时可遇;宿泊费
    //      干肉100+毛100+火把1(旗标代价)换一堂课,三选一 Perk,
    //      已学的灰显(原版 available 隐藏) ----
    [DR_EV_MASTER] = {
        .text_id = DR_T_EV_MASTER, .scene_mask = DR_SCENE_HOME,
        .priority = 10, .cooldown_s = 600,
        .conds = { C(DR_COND_FLAG_SET, DR_FLAG_COMPASS, 0),
                   COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            { .text_id = DR_T_EV_MASTER_AGREE,
              .avail_flag_clear = DR_COST_NONE,
              .cost_res = { DR_RES_FOOD, DR_RES_FUR, DR_COST_FLAG },
              .cost_amt = { 100, 100, DR_FLAG_TORCH },
              .effects = { E(DR_EFF_GOTO_EVENT, DR_EV_MASTER_AGREE, 0),
                           EFF_NONE, EFF_NONE, EFF_NONE } },
            FREE_CH(DR_T_EV_SEND_AWAY, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_MASTER_AGREE] = {
        .text_id = DR_T_EV_MASTER_ROOM, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            { .text_id = DR_T_EV_MASTER_EVASION,
              .avail_flag_clear = DR_FLAG_PERK_EVASIVE,
              .cost_res = { DR_COST_NONE, DR_COST_NONE, DR_COST_NONE },
              .cost_amt = { 0, 0, 0 },
              .effects = { E(DR_EFF_FLAG_SET, DR_FLAG_PERK_EVASIVE, 0),
                           EFF_NONE, EFF_NONE, EFF_NONE } },
            { .text_id = DR_T_EV_MASTER_PRECISION,
              .avail_flag_clear = DR_FLAG_PERK_PRECISE,
              .cost_res = { DR_COST_NONE, DR_COST_NONE, DR_COST_NONE },
              .cost_amt = { 0, 0, 0 },
              .effects = { E(DR_EFF_FLAG_SET, DR_FLAG_PERK_PRECISE, 0),
                           EFF_NONE, EFF_NONE, EFF_NONE } },
            { .text_id = DR_T_EV_MASTER_FORCE,
              .avail_flag_clear = DR_FLAG_PERK_BARBARIAN,
              .cost_res = { DR_COST_NONE, DR_COST_NONE, DR_COST_NONE },
              .cost_amt = { 0, 0, 0 },
              .effects = { E(DR_EFF_FLAG_SET, DR_FLAG_PERK_BARBARIAN, 0),
                           EFF_NONE, EFF_NONE, EFF_NONE } },
            FREE_CH(DR_T_EV_MASTER_NOTHING, EFF_NONE, EFF_NONE, EFF_NONE,
                    EFF_NONE),
        },
        .choice_count = 4,
    },
    // ---- 执行者(M5;原版 events/executioner.js 的星舰守卫,设备适配简化为
    //      首访星舰页触发:给 1 异星合金 = 和解(制造台解锁)/ 拒绝 = 遭遇战,
    //      战斗由 UI 桥接启动(事件引擎够不着战斗态),敌人取士兵表) ----
    [DR_EV_EXEC] = {
        .text_id = DR_T_EV_EXEC, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            COST_CH(DR_T_EV_EXEC_GIVE, DR_RES_ALIEN, 1,
                E(DR_EFF_EXEC_SET, 0, 1),
                E(DR_EFF_GOTO_EVENT, DR_EV_EXEC_PEACE, 0),
                EFF_NONE, EFF_NONE),
            FREE_CH(DR_T_EV_EXEC_REFUSE, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 2,
    },
    [DR_EV_EXEC_PEACE] = {
        .text_id = DR_T_EV_EXEC_PEACE, .scene_mask = 0,
        .priority = 0, .cooldown_s = 0,
        .conds = { COND_NONE, COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            FREE_CH(DR_T_UI_BACK, EFF_NONE, EFF_NONE, EFF_NONE, EFF_NONE),
        },
        .choice_count = 1,
    },
};

const dr_event_t *dr_events_table(uint16_t *count) {
    if (count) *count = sizeof(k_events) / sizeof(k_events[0]);
    return k_events;
}
