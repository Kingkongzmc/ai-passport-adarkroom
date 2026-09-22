// main/game_darkroom/dr_text.c —— 文案表。M1 教学自测用;M6 全量灌装。
#include "dr_text.h"

static const char *k_texts[] = {
    [DR_T_UI_TITLE]        = "小黑屋",
    [DR_T_UI_BACK]         = "返回",
    [DR_T_UI_CONFIRM]      = "确定",
    [DR_T_ACT_FIRE]        = "生火",
    [DR_T_ACT_GATHER]      = "收集木材",
    [DR_T_EV_STRANGER_1]   = "门被推开,一个陌生的女人站在寒风里。她说她可以帮忙看火。",
    [DR_T_EV_STRANGER_1_C0] = "让她留下",
    [DR_T_EV_STRANGER_1_C1] = "赶走她",
    [DR_T_EV_DEBUG]        = "引擎自测事件",
};

const char *dr_text(uint16_t id) {
    if (id == 0 || id >= sizeof(k_texts) / sizeof(k_texts[0])) return "";
    return k_texts[id];
}
