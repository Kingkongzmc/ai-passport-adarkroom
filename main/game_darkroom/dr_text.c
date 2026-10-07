// main/game_darkroom/dr_text.c —— 文案表。界面 + M6 事件文案。
// 改动本表后必须重跑 tools/gen_fonts.sh 重生成闭集字库(I24 教训)。
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

    // ---- M6 事件文案(原版 room.js/outside.js 事件,中文重写) ----
    [DR_T_EV_IGNORE]       = "不予理会",
    [DR_T_EV_SEND_AWAY]    = "打发走",
    [DR_T_EV_LET_GO]       = "放任不管",
    [DR_T_EV_GIVE_FUR]     = "给毛",
    [DR_T_EV_GIVE_MORE_FUR] = "多给些毛",
    [DR_T_EV_GIVE_WOOD]    = "给木",
    [DR_T_EV_GIVE_MORE_WOOD] = "多给些木",
    [DR_T_EV_GIVE_MED]     = "给药",
    [DR_T_EV_USE_MED]      = "用药",
    [DR_T_EV_BUY_MED]      = "买药",

    [DR_T_EV_NOISE_O]      = "墙外传来窸窸窣窣的动静,听不出是什么东西。",
    [DR_T_EV_NOISE_O_C0]   = "出去看看",
    [DR_T_EV_NOISE_O_STUFF] = "门槛外放着一捆木柴,裹在粗砺的毛皮里。夜重新静了下来。",
    [DR_T_EV_NOISE_O_NOTHING] = "模糊的影子一闪而过。动静停了。",

    [DR_T_EV_NOISE_I]      = "库房里传来抓挠声。有什么东西钻了进去。",
    [DR_T_EV_NOISE_I_C0]   = "打开看看",
    [DR_T_EV_NOISE_I_S]    = "木柴少了一些,地上落满细小的鳞片。",
    [DR_T_EV_NOISE_I_T]    = "木柴少了一些,地上落满细小的牙齿。",
    [DR_T_EV_NOISE_I_C]    = "木柴少了一些,地上散着几条碎布。",

    [DR_T_EV_BEGGAR]       = "一个乞丐走上前来,讨几件毛皮过夜。",
    [DR_T_EV_BEGGAR_S]     = "他连声道谢,留下一小堆发亮的鳞片。",
    [DR_T_EV_BEGGAR_T]     = "他连声道谢,留下一小堆尖利的牙齿。",
    [DR_T_EV_BEGGAR_C]     = "他连声道谢,留下几条还算完整的布。",

    [DR_T_EV_SHADY]        = "一个行迹可疑的工匠路过,说便宜点也能帮你盖间小屋。",
    [DR_T_EV_SHADY_C0]     = "付钱动工",
    [DR_T_EV_SHADY_C1]     = "回绝他",
    [DR_T_EV_SHADY_BUILT]  = "工匠手脚麻利,一间小屋立了起来。",
    [DR_T_EV_SHADY_STOLEN] = "第二天清晨,工匠和你的木料都不见了。",

    [DR_T_EV_WAND_W]       = "旅人拉着空车停在门口:给他木料,他会带回更多。建造者不太信他。",
    [DR_T_EV_WAND_BACK_W]  = "旅人回来了,车上的木料堆得冒尖。",
    [DR_T_EV_WAND_GONE]    = "旅人再也没有出现。",
    [DR_T_EV_WAND_F]       = "旅人拉着空车停在门口:给她毛皮,她会带回更多。建造者不太信她。",
    [DR_T_EV_WAND_BACK_F]  = "旅人回来了,车上的毛皮堆得冒尖。",

    [DR_T_EV_SICKMAN]      = "一个男人咳着嗽挪到门口,乞求一剂药。",
    [DR_T_EV_SICKMAN_ALLOY] = "他留下谢礼:旅行中捡到的一块怪金属。",
    [DR_T_EV_SICKMAN_SC]   = "他掏空了口袋,只有几片鳞。",
    [DR_T_EV_SICKMAN_NOTHING] = "他道过谢,蹒跚着走远了。",

    [DR_T_EV_TRAPS]        = "几个陷阱被扯得稀烂。巨大的脚印一路延伸进森林。",
    [DR_T_EV_TRAPS_C0]     = "追踪它",
    [DR_T_EV_TRAPS_C1]     = "不管它",
    [DR_T_EV_TRAPS_CATCH]  = "不远处的树丛里卧着一头巨兽,几乎没做抵抗。收获丰厚。",
    [DR_T_EV_TRAPS_LOST]   = "脚印走出几十步就消失了。森林一片寂静。",

    [DR_T_EV_FIRE]         = "大火吞没了一间小屋。屋里的住客没能逃出来。",
    [DR_T_EV_FIRE_C0]      = "哀悼",

    [DR_T_EV_SICK]         = "疫病在村里蔓延开来。急需药品。",
    [DR_T_EV_SICK_HEALED]  = "疫病被及时止住了。",
    [DR_T_EV_SICK_DEATH]   = "接连几天都在下葬。夜里满是哭声。",

    [DR_T_EV_PLAGUE]       = "一场可怕的瘟疫正在村里肆虐。急需药品。",
    [DR_T_EV_PLAGUE_KEPT]  = "瘟疫终于被压了下去。只有几个人没能挺过来。",
    [DR_T_EV_PLAGUE_RAGE]  = "瘟疫撕碎了村庄。夜里全是惨叫,只求死得快些。",

    [DR_T_EV_BEAST]        = "一群野兽从林子里扑出来。战斗短暂而血腥,野兽被击退,村民也有死伤。",
    [DR_T_EV_BEAST_C0]     = "回家",

    // ---- M6 切片二(原版 room.js/global.js/outside.js 事件,中文重写) ----
    [DR_T_EV_RAID]         = "一声枪响划过林梢。一队武装的人冲出森林,朝人群开火。他们最终被驱退,村里却添了几座新坟。",

    [DR_T_EV_NOMAD]        = "游牧商人背着大包小包挪过来,不肯说来处,也不打算久留。他嘟囔:陷阱装上饵,收获才像样。",
    [DR_T_EV_NOMAD_SCALES] = "买鳞",
    [DR_T_EV_NOMAD_TEETH]  = "买牙",
    [DR_T_EV_NOMAD_BAIT]   = "买饵",
    [DR_T_EV_NOMAD_COMPASS] = "买罗盘",
    [DR_T_EV_GOODBYE]      = "告别",

    [DR_T_EV_THIEF]        = "村民从库房里揪出一个脏兮兮的男人,说他的同伙一直在偷库存,嚷着要把他吊起来示众。",
    [DR_T_EV_THIEF_HANG]   = "吊死他",
    [DR_T_EV_THIEF_HANGED] = "窃贼被高高地吊在库房门前。杀鸡儆猴,前些阵子不翼而飞的物资陆陆续续都回来了。",
    [DR_T_EV_THIEF_SPARE]  = "放过他",
    [DR_T_EV_THIEF_SPARED] = "他说他感激不迭,往后不会再来了。临走前,他把一身翻墙越货、不留影踪的本事教给了你。",

    [DR_T_EV_SCOUT]        = "斥候说她走遍了这片荒野。这些见闻可以卖给你,但得先给点好处。",
    [DR_T_EV_SCOUT_MAP]    = "买地图",
    [DR_T_EV_SCOUT_LEARN]  = "学侦查",

    [DR_T_EV_MASTER]       = "一位老流浪者上门。他笑容可掬,讨一晚宿处。",
    [DR_T_EV_MASTER_AGREE] = "留宿他",
    [DR_T_EV_MASTER_ROOM]  = "火边一夜,老人愿意倾囊相授。挑一样吧。",
    [DR_T_EV_MASTER_EVASION] = "学闪避",
    [DR_T_EV_MASTER_PRECISION] = "学精准",
    [DR_T_EV_MASTER_FORCE] = "学蛮力",
    [DR_T_EV_MASTER_NOTHING] = "不必了",
};

const char *dr_text(uint16_t id) {
    if (id == 0 || id >= sizeof(k_texts) / sizeof(k_texts[0])) return "";
    return k_texts[id] ? k_texts[id] : "";   // 指定初始化的空洞兜底
}
