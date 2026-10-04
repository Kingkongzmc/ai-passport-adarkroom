// main/game_darkroom/dr_text.h —— 文案索引(id → UTF-8 字符串)。
// 界面文案与事件文案(M6 灌装)共用此表;改文案后必须重跑 tools/gen_fonts.sh。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 文案 id 空间(事件标题/选项复用同一表;分段按需扩)。
enum {
    DR_T_UI_TITLE = 1,     // 《小黑屋》
    DR_T_UI_BACK,          // 返回
    DR_T_UI_CONFIRM,       // 确定
    DR_T_ACT_FIRE,         // 生火
    DR_T_ACT_GATHER,       // 收集木材
    DR_T_EV_STRANGER_1,    // 开场剧情:陌生人到来(测试用)
    DR_T_EV_STRANGER_1_C0, //   选项:让她留下
    DR_T_EV_STRANGER_1_C1, //   选项:赶走她
    DR_T_EV_DEBUG,         // 引擎自测事件

    // ---- M6 事件文案(原版 events/room.js + outside.js 重写) ----
    // 通用选项(多事件共享;数量由代价后缀显示,文本不带数字)
    DR_T_EV_IGNORE,        // 不予理会
    DR_T_EV_SEND_AWAY,     // 打发走
    DR_T_EV_LET_GO,        // 放任不管
    DR_T_EV_GIVE_FUR,      // 给毛
    DR_T_EV_GIVE_MORE_FUR, // 多给些毛
    DR_T_EV_GIVE_WOOD,     // 给木
    DR_T_EV_GIVE_MORE_WOOD,// 多给些木
    DR_T_EV_GIVE_MED,      // 给药
    DR_T_EV_USE_MED,       // 用药
    DR_T_EV_BUY_MED,       // 买药
    // 墙外动静(Noises Outside:30% 得木+毛)
    DR_T_EV_NOISE_O, DR_T_EV_NOISE_O_C0,
    DR_T_EV_NOISE_O_STUFF, DR_T_EV_NOISE_O_NOTHING,
    // 库房异响(Noises Inside:木换鳞/牙/布)
    DR_T_EV_NOISE_I, DR_T_EV_NOISE_I_C0,
    DR_T_EV_NOISE_I_S, DR_T_EV_NOISE_I_T, DR_T_EV_NOISE_I_C,
    // 乞讨者(The Beggar:毛皮换鳞/牙/布)
    DR_T_EV_BEGGAR, DR_T_EV_BEGGAR_S, DR_T_EV_BEGGAR_T, DR_T_EV_BEGGAR_C,
    // 阴险的工匠(The Shady Builder:300 木赌小屋)
    DR_T_EV_SHADY, DR_T_EV_SHADY_C0, DR_T_EV_SHADY_C1,
    DR_T_EV_SHADY_BUILT, DR_T_EV_SHADY_STOLEN,
    // 神秘的流浪者(The Mysterious Wanderer:木/毛赌局)
    DR_T_EV_WAND_W, DR_T_EV_WAND_BACK_W, DR_T_EV_WAND_GONE,
    DR_T_EV_WAND_F, DR_T_EV_WAND_BACK_F,
    // 病人(The Sick Man:药换异金/鳞)
    DR_T_EV_SICKMAN, DR_T_EV_SICKMAN_ALLOY, DR_T_EV_SICKMAN_SC,
    DR_T_EV_SICKMAN_NOTHING,
    // 毁坏的陷阱(A Ruined Trap:损半陷阱,追踪赌战利品)
    DR_T_EV_TRAPS, DR_T_EV_TRAPS_C0, DR_T_EV_TRAPS_C1,
    DR_T_EV_TRAPS_CATCH, DR_T_EV_TRAPS_LOST,
    // 小屋失火(Fire:毁一屋减 4 人)
    DR_T_EV_FIRE, DR_T_EV_FIRE_C0,
    // 疫病(Sickness:药或减员 25%)
    DR_T_EV_SICK, DR_T_EV_SICK_HEALED, DR_T_EV_SICK_DEATH,
    // 大瘟疫(Plague:买药/五剂药/减员 50%)
    DR_T_EV_PLAGUE, DR_T_EV_PLAGUE_KEPT, DR_T_EV_PLAGUE_RAGE,
    // 野兽袭击(A Beast Attack:减员 5,得毛/肉/牙)
    DR_T_EV_BEAST, DR_T_EV_BEAST_C0,
};

const char *dr_text(uint16_t id);

#ifdef __cplusplus
}
#endif
