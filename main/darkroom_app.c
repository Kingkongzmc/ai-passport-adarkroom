// main/darkroom_app.c —— 《小黑屋》应用壳 v7(全量重写,替代补丁版)。
//
// 架构:每页一个 layout 函数,无条件写入该页所有对象的
// 位置/尺寸/文字/可见性——没有"复位"、没有"跨页借用"、没有"从上一页
// 继承",切页 = 整页属性全量重写,从架构上杜绝布局漂移。
//
// 布局与 tools/mockup.html 切片一一对应:
//   scr 240×320 > panel 216×296@(12,12)(边框1+pad8,圆角4)
//   > content 198×278 > 元素(全部显式坐标,焦点环 = outline 金 1px)
//
// 按键:PRESS 按下沿(上/下/确定),长按确定=返回;渲染后 60ms 静默期丢弃
// 幽灵按键;不在按键上下文读 ADC。存档延迟到 LVGL 锁外落盘。
// 事件触发必须走 page_goto(直接赋 s.page 曾致"隐形事件页")。
//
// 硬性规则:全应用禁止 LV_LABEL_LONG_WRAP。真机上隐藏处于/含 wrap 标签的
// 对象会卡死 LVGL 渲染任务(roulette_app.c 同类事故),故一切标签 LONG_DOT,
// 需要折行的文案(弹窗正文)由 wrap_cjk() 预折行成显式 '\n'。
// 共享控件(资源格/日志/动作行)无"默认位置":谁显示谁全量重写排版,
// 防止上一页改过的坐标/尺寸泄漏到下一页。
#include "darkroom_app.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "dr_events.h"
#include "dr_port.h"
#include "dr_text.h"
#include "dr_util.h"
#include "dr_world.h"
#include "lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

LV_FONT_DECLARE(dr_font_12);
LV_FONT_DECLARE(dr_font_16);
LV_FONT_DECLARE(dr_font_24);

// ---- 配色(与 mockup 一致) ----
#define COL_BG     0x0D0D0D
#define COL_PANEL  0x1C1917
#define COL_BORDER 0x57534E
#define COL_TEXT   0xE7E5E4
#define COL_DIM    0xA8A29E
#define COL_GOLD   0xFFD928
#define COL_CD     0x3A3A44
#define COL_LOCK   0x57534E
#define COL_ME     0x4A90D9
#define COL_ENEMY  0xE43B2F

// ---- 布局常量(全屏面板 + 居中内容区) ----
#define PAN_W   240
#define PAN_H   320
#define CTN_W   198      // 内容区宽(设计稿切片宽)
#define CTN_H   278      // 内容区高(设计稿切片高)
#define CTN_X   13       // (240-2*8-198)/2:内容区水平居中于面板
#define CTN_Y   13       // (320-2*8-278)/2:内容区垂直居中于面板
#define ROW_W   198
#define ROW_H   22
#define DROW_W  172      // 弹窗内选项行宽 = 196-4-20
#define LOG_MAX 5        // 主页日志行数
#define LOG_VIS 8        // 主页日志可视行数上限(逐行渐隐)

static const char *TAG = "darkroom";

typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_event_t;

// ---- 页面 ----
typedef enum {
    PG_TITLE = 0, PG_HOME, PG_BUILD, PG_VILLAGE, PG_MAP,    PG_RUIN, PG_COMBAT, PG_TRADE, PG_SETTINGS,
    PG_EVENT, PG_CONFIRM, PG_CRAFT,
} page_t;

// ---- 状态 ----
static struct {
    uint32_t boot_ms;          // 开机防抖基准
    uint32_t last_render_ms;   // 渲染静默期基准
    volatile bool save_pending;
    uint8_t confirm_from;      // 确认页来源(0=设置重开 1=贸易全卖)
    int8_t nav_focus;          // 主页导航焦点(-1=动作区,0..3=tab)
    int8_t village_adj;        // 村庄调节模式:在调的职业(-1=未调节)
    uint8_t job_scroll;        // 村庄职业区滚动偏移(窗口 5 行)
    uint8_t craft_row_map[16]; // 制造页:行号 → 制造项
    uint8_t craft_row_count;   // 制造页:可见制造行数(不含返回)
    bool prev_gather_ready;    // 上一拍冷却秒数(变化即重绘:冷却条/倒计时逐拍走)
    bool prev_trap_ready;
    bool prev_stoke_active;    // 主页添柴冷却条活动态(结束瞬间也要重绘一次)
    uint32_t autosave_ms;      // 周期存档基准(挂机产出也落盘,断电回滚≤1分钟)
    uint32_t embark_cd_ms;     // 出发冷却到期时刻(死亡后 120s,原版口径)
    int8_t outfit_adj;         // 出发整备:正在调配的物资(-1=无)
    dr_game_t game;
    dr_rules_rt_t rules_rt;
    dr_event_session_t ev_sess;
    page_t page;
    int focus;
    uint16_t ev_idx;
    bool dirty;
} s;

// ---- UI 对象 ----
static lv_obj_t *s_scr, *s_panel, *s_content;
static lv_obj_t *s_topbar, *s_batt;
static lv_obj_t *s_tabs[4], *s_tablbl[4];
static lv_obj_t *s_cells[8];
static lv_obj_t *s_loglines[LOG_VIS];
static lv_obj_t *s_hint;
// 动作行:容器 + 冷却填充 + 箭头 + 文字(主页/战斗/弹窗选项共用骨架)
typedef struct {
    lv_obj_t *row, *fill, *arrow, *lbl;
} actrow_t;
#define ACT_ROWS 4
static actrow_t s_acts[ACT_ROWS];
// 列表行:箭头 + 名称 + 值(建造/村庄/贸易/设置/地图目的地/房间)
typedef struct {
    lv_obj_t *row, *mark, *t, *v;
} listrow_t;
#define LIST_ROWS 10
static listrow_t s_list[LIST_ROWS];
static dr_world_t s_world;   // 世界与远征运行态(会话内;种子派生,不落盘)
// 弹窗层
static lv_obj_t *s_veil, *s_dpanel, *s_dtitle, *s_dbody;
static actrow_t s_dacts[4];
// 地图/房间格阵 + 图例 + 血条
#define MAP_CELLS 81
static lv_obj_t *s_map[MAP_CELLS];
static lv_obj_t *s_legend[2];
#define ROOM_CELLS 9
static lv_obj_t *s_room[ROOM_CELLS];
static lv_obj_t *s_hp[2], *s_hpfill[2];

static lv_timer_t *s_timer;
static QueueHandle_t s_key_queue;
static TaskHandle_t s_key_task;
static volatile bool s_key_quit;

#define LOG_LINES 8        // 主页日志环形缓冲条数
static char s_logs[LOG_LINES][48];
static int s_log_cnt;

static void log_push(const char *text) {
    if (s_log_cnt == LOG_LINES)
        memmove(s_logs[0], s_logs[1], sizeof(s_logs) - sizeof(s_logs[0]));
    else
        s_log_cnt++;
    snprintf(s_logs[s_log_cnt - 1], sizeof(s_logs[0]), "%s", text);
    s.dirty = true;
}

// ---- 日志随档持久化(独立 NVS 键 dr_log;重启后主页日志原样恢复) ----
// 载荷 = 行数(1B) + 行数×48B(环形缓冲恒紧凑在 [0,cnt))。坏键 = 空日志。
// 魔数带代际("DLO2"):字库是闭集,旧代固件日志里的字符可能已不在字库内,
// 恢复会显示乱码——跨代日志一律忽略(玩法重做后旧机制日志无保留价值),
// 下次存档即被新格式覆盖。
#pragma pack(push, 1)
typedef struct { uint32_t magic; uint32_t crc; } dr_log_hdr_t;
#pragma pack(pop)
#define DR_LOG_MAGIC 0x324F4C44u   // "DLO2"(DLO1=旧代,弃)

static size_t log_pack(void *buf, size_t cap) {
    size_t payload = 1u + (size_t)s_log_cnt * sizeof(s_logs[0]);
    if (!buf || cap < sizeof(dr_log_hdr_t) + payload) return 0;
    dr_log_hdr_t *h = (dr_log_hdr_t *)buf;
    h->magic = DR_LOG_MAGIC;
    uint8_t *p = (uint8_t *)buf + sizeof(*h);
    *p++ = (uint8_t)s_log_cnt;
    memcpy(p, s_logs, payload - 1u);
    h->crc = dr_crc32((const uint8_t *)buf + sizeof(*h), payload);
    return sizeof(dr_log_hdr_t) + payload;
}

static bool log_restore(const void *buf, size_t len) {
    if (len < sizeof(dr_log_hdr_t) + 1u) return false;
    const dr_log_hdr_t *h = (const dr_log_hdr_t *)buf;
    const uint8_t *p = (const uint8_t *)buf + sizeof(*h);
    size_t payload = len - sizeof(*h);
    if (h->magic != DR_LOG_MAGIC || payload < 1u || payload > 1u + sizeof(s_logs))
        return false;
    if (h->crc != dr_crc32(p, payload)) return false;
    uint8_t cnt = *p++;
    if (cnt > LOG_LINES || 1u + (size_t)cnt * sizeof(s_logs[0]) != payload)
        return false;
    memcpy(s_logs, p, payload - 1u);
    for (int i = 0; i < cnt; i++)    // 行尾兜底 NUL,防越界渲染
        s_logs[i][sizeof(s_logs[0]) - 1] = '\0';
    s_log_cnt = cnt;
    return true;
}

// 统一存档点:主档与日志键同写(动作触发/周期/退出共用)
static void save_all(void) {
    dr_port_save(&s.game);
    uint8_t lbuf[sizeof(dr_log_hdr_t) + 1u + sizeof(s_logs)];
    size_t n = log_pack(lbuf, sizeof(lbuf));
    if (n) dr_port_log_save(lbuf, n);
}

static const char *fire_char(void) {
    static const char *f[] = {"熄", "微", "跳", "旺", "炽"};
    uint8_t lv = s.game.fire_lv;
    if (lv > DR_FIRE_ROARING) lv = DR_FIRE_ROARING;
    return f[lv];
}

static const char *bld_short(int id) {
    static const char *n[] = {"板车", "陷阱", "小屋", "猎屋", "贸站", "革坊",
                              "熏房", "钢厂", "械库"};
    return (id >= 0 && id < 9) ? n[id] : "建筑";
}

// 村庄页签门:森林剧情解锁(原版 unlockForest)
static bool forest_open(void);
static bool build_affordable(uint8_t id);   // 建造行足额可付判定
static bool trade_affordable(uint8_t item); // 贸易行支付能力判定(灰显同源)
static bool nav_tab_enabled(int i);         // 页签启用表(荒野门)

// 第二页签名(原版 a silent forest → village):建小屋前是"森林",之后是"村庄"
static const char *outside_name(void) {
    return s.game.building_lv[DR_BLD_HUT] > 0 ? "村庄" : "森林";
}

static lv_obj_t *label_new(lv_obj_t *parent, const lv_font_t *font,
                           lv_color_t color, int x, int y, int w, int h) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

// 焦点样式 = 金色文字 + 行首固定宽度箭头列(独立 label,">"/空)。
// 前缀不在正文里:比例字体下 "> " 与两空格不等宽,会导致正文横移。

// 箭头列宽(f12 取 10,f16 取 12),正文 x 统一 = 行 x + 箭头列宽。
#define ARROW_W12 10
#define ARROW_W16 12

// 手工折行:按显示宽度断行(ASCII 半格、其余整格),插入 '\n'。
// 弹窗正文 f16 在 176px 内每行 line_units 格;配合 LONG_DOT 使用,
// 不经 LVGL 的 WRAP 路径(见文件头硬性规则)。
static void wrap_cjk(char *out, size_t outsz, const char *src, int line_units) {
    size_t o = 0;
    int units = 0;
    while (*src && o + 4 < outsz) {
        uint8_t c = (uint8_t)*src;
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        int u = (c < 0x80) ? 1 : 2;
        if (c == '\n') {                  // 源文本自带断行,原样保留
            out[o++] = '\n';
            src += len;
            units = 0;
            continue;
        }
        if (units + u > line_units) {
            out[o++] = '\n';
            units = 0;
            while (*src == ' ') src++;    // 行首空格丢弃
            continue;
        }
        for (int i = 0; i < len && *src; i++) out[o++] = *src++;
        units += u;
    }
    out[o] = 0;
}

static void set_dbody(const char *text) {
    static char wrapped[256];
    // 176px / 16px = 11 个整格字;单位为半角格(汉字占 2),故传 22
    wrap_cjk(wrapped, sizeof(wrapped), text, 22);
    lv_label_set_text(s_dbody, wrapped);
}

// ===================================================================
// build_ui:一次性创建全部对象(含隐藏),页面渲染只改属性与可见性
// ===================================================================
static lv_obj_t *s_t_big, *s_t_sub;    // 标题页大标题/副文案(专用)
static lv_obj_t *s_t_ok;

static void build_ui(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);

    s_panel = lv_obj_create(s_scr);
    lv_obj_set_pos(s_panel, 0, 0);
    lv_obj_set_size(s_panel, PAN_W, PAN_H);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);   // 无外框:铺满全屏
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_scroll_to(s_panel, 0, 0, LV_ANIM_OFF);   // 滚动归零,防残留偏移

    s_content = lv_obj_create(s_panel);
    lv_obj_set_pos(s_content, CTN_X, CTN_Y);
    lv_obj_set_size(s_content, CTN_W, CTN_H);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_OFF);

    // 顶栏:标题(0,0,110,20 f16) + 电池(150,2,48,15 f12 右对齐)
    s_topbar = label_new(s_content, &dr_font_16, lv_color_hex(COL_GOLD), 0, 0, 110, 20);
    lv_label_set_text(s_topbar, "小黑屋");
    s_batt = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 150, 2, 48, 15);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_batt, "--%");

    // 场景导航栏 4×47×20 @ y=24,文字 + 底部指示线
    static const char *tab_names[4] = {"小屋", "村庄", "荒野", "≡"};
    for (int i = 0; i < 4; i++) {
        s_tabs[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_tabs[i], i * 50, 24);
        lv_obj_set_size(s_tabs[i], 47, 20);
        lv_obj_set_style_bg_opa(s_tabs[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tabs[i], 0, 0);
        lv_obj_set_style_border_side(s_tabs[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_tabs[i], 1, 0);
        lv_obj_set_style_border_color(s_tabs[i], lv_color_hex(0x3A3A44), 0);
        lv_obj_clear_flag(s_tabs[i], LV_OBJ_FLAG_SCROLLABLE);
        s_tablbl[i] = lv_label_create(s_tabs[i]);
        lv_obj_center(s_tablbl[i]);
        lv_obj_set_style_text_font(s_tablbl[i], &dr_font_12, 0);
        lv_obj_set_style_text_color(s_tablbl[i], lv_color_hex(COL_DIM), 0);
        lv_label_set_text(s_tablbl[i], tab_names[i]);
    }

    // 资源带 8 格:列 x=0/49/99/148,行 y=48/63,46×15
    for (int i = 0; i < 8; i++) {
        s_cells[i] = label_new(s_content, &dr_font_12, lv_color_hex(COL_TEXT),
                               (i % 4) * 49, 48 + (i / 4) * 15, 46, 15);
    }

    // 日志逐行标签(0,90 起,行高自适应;位置/显隐/颜色由 render_home_log 管理)
    for (int i = 0; i < LOG_VIS; i++) {
        s_loglines[i] = label_new(s_content, &dr_font_12,
                                  lv_color_hex(COL_DIM), 0, 90 + i * 15, 198, 15);
        lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 页面底部说明行
    s_hint = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 0, 256, 198, 15);

    // 动作行 4 行 @ y=184/208/232/256
    for (int i = 0; i < ACT_ROWS; i++) {
        actrow_t *a = &s_acts[i];
        a->row = lv_obj_create(s_content);
        lv_obj_set_pos(a->row, 0, 184 + i * 24);
        lv_obj_set_size(a->row, ROW_W, ROW_H);
        lv_obj_set_style_radius(a->row, 3, 0);
        lv_obj_set_style_bg_opa(a->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(a->row, 0, 0);
        lv_obj_set_style_pad_all(a->row, 0, 0);
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_SCROLLABLE);
        a->fill = lv_obj_create(a->row);
        lv_obj_set_pos(a->fill, 1, 1);
        lv_obj_set_size(a->fill, 0, 20);
        lv_obj_set_style_radius(a->fill, 2, 0);
        lv_obj_set_style_bg_color(a->fill, lv_color_hex(COL_CD), 0);
        lv_obj_set_style_border_width(a->fill, 0, 0);
        lv_obj_set_style_pad_all(a->fill, 0, 0);
        lv_obj_clear_flag(a->fill, LV_OBJ_FLAG_SCROLLABLE);
        a->arrow = label_new(a->row, &dr_font_16, lv_color_hex(COL_GOLD),
                             2, 1, ARROW_W16, 20);
        a->lbl = label_new(a->row, &dr_font_16, lv_color_hex(COL_TEXT),
                           2 + ARROW_W16, 1, 186 - ARROW_W16 - 4, 20);
    }

    // 列表行 9 行(y 由页面渲染时设定)
    for (int i = 0; i < LIST_ROWS; i++) {
        listrow_t *r = &s_list[i];
        r->row = lv_obj_create(s_content);
        lv_obj_set_size(r->row, ROW_W, ROW_H);
        lv_obj_set_style_radius(r->row, 3, 0);
        lv_obj_set_style_bg_opa(r->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r->row, 0, 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
        r->mark = label_new(r->row, &dr_font_12, lv_color_hex(COL_GOLD),
                            4, 1, ARROW_W12, 20);
        r->t = label_new(r->row, &dr_font_12, lv_color_hex(COL_TEXT),
                         4 + ARROW_W12, 1, 140, 20);
        r->v = label_new(r->row, &dr_font_12, lv_color_hex(COL_DIM), 102, 1, 90, 20);
        lv_obj_set_style_text_align(r->v, LV_TEXT_ALIGN_RIGHT, 0);
    }

    // 弹窗层:遮罩 + 面板(196×236 @(10,42),圆角4 金边2)
    s_veil = lv_obj_create(s_scr);
    lv_obj_set_pos(s_veil, 0, 0);
    lv_obj_set_size(s_veil, 240, 320);
    lv_obj_set_style_bg_color(s_veil, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_veil, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_veil, 0, 0);
    lv_obj_set_style_pad_all(s_veil, 0, 0);
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);

    s_dpanel = lv_obj_create(s_scr);
    lv_obj_set_pos(s_dpanel, 22, 42);   // (240-196)/2=22,水平居中
    lv_obj_set_size(s_dpanel, 196, 236);
    lv_obj_set_style_pad_all(s_dpanel, 10, 0);
    lv_obj_set_style_bg_color(s_dpanel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(s_dpanel, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(s_dpanel, 2, 0);
    lv_obj_set_style_radius(s_dpanel, 4, 0);
    lv_obj_clear_flag(s_dpanel, LV_OBJ_FLAG_SCROLLABLE);

    s_dtitle = label_new(s_dpanel, &dr_font_24, lv_color_hex(COL_GOLD), 0, 0, 176, 30);
    lv_obj_add_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    s_dbody = label_new(s_dpanel, &dr_font_16, lv_color_hex(COL_TEXT), 0, 0, 176, 100);

    // 弹窗选项行 4 行 172 宽(页面渲染时定位)
    for (int i = 0; i < 4; i++) {
        actrow_t *a = &s_dacts[i];
        a->row = lv_obj_create(s_dpanel);
        lv_obj_set_size(a->row, DROW_W, ROW_H);
        lv_obj_set_style_radius(a->row, 3, 0);
        lv_obj_set_style_bg_opa(a->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(a->row, 0, 0);
        lv_obj_set_style_pad_all(a->row, 0, 0);
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_SCROLLABLE);
        a->fill = lv_obj_create(a->row);
        lv_obj_set_pos(a->fill, 1, 1);
        lv_obj_set_size(a->fill, 0, 20);
        lv_obj_set_style_radius(a->fill, 2, 0);
        lv_obj_set_style_bg_color(a->fill, lv_color_hex(COL_CD), 0);
        lv_obj_set_style_border_width(a->fill, 0, 0);
        lv_obj_set_style_pad_all(a->fill, 0, 0);
        lv_obj_clear_flag(a->fill, LV_OBJ_FLAG_SCROLLABLE);
        a->arrow = label_new(a->row, &dr_font_16, lv_color_hex(COL_GOLD),
                             2, 1, ARROW_W16, 20);
        a->lbl = label_new(a->row, &dr_font_16, lv_color_hex(COL_TEXT),
                           2 + ARROW_W16, 1, 160 - ARROW_W16 - 4, 20);
    }

    // 标题页三件套(独立对象)
    s_t_big = label_new(s_content, &dr_font_24, lv_color_hex(COL_GOLD), 0, 100, 200, 30);
    lv_obj_set_style_text_align(s_t_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_t_big, "小黑屋");
    s_t_sub = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 0, 136, 200, 15);
    lv_obj_set_style_text_align(s_t_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_t_sub, "屋里冷得刺骨");
    s_t_ok = lv_obj_create(s_content);
    lv_obj_set_pos(s_t_ok, 40, 180);
    lv_obj_set_size(s_t_ok, 120, ROW_H);
    lv_obj_set_style_radius(s_t_ok, 3, 0);
    lv_obj_set_style_bg_opa(s_t_ok, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_t_ok, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(s_t_ok, 1, 0);
    lv_obj_set_style_pad_all(s_t_ok, 0, 0);
    lv_obj_clear_flag(s_t_ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tok = label_new(s_t_ok, &dr_font_16, lv_color_hex(COL_GOLD), 0, 1, 120, 20);
    lv_obj_set_style_text_align(tok, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(tok, "OK键开始");

    // 地图 9×9 格 @(50,44) 格10px 间隔1
    for (int i = 0; i < MAP_CELLS; i++) {
        s_map[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_map[i], 50 + (i % 9) * 11, 44 + (i / 9) * 11);
        lv_obj_set_size(s_map[i], 10, 10);
        lv_obj_set_style_radius(s_map[i], 0, 0);
        lv_obj_set_style_border_width(s_map[i], 1, 0);
        lv_obj_set_style_border_color(s_map[i], lv_color_hex(0x33302C), 0);
        lv_obj_set_style_bg_color(s_map[i], lv_color_hex(COL_BG), 0);
        lv_obj_set_style_pad_all(s_map[i], 0, 0);
    }
    // 图例两行 @ y=146/161(居中)
    for (int i = 0; i < 2; i++) {
        s_legend[i] = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM),
                                0, 146 + i * 15, 198, 15);
        lv_obj_set_style_text_align(s_legend[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // 房间 3×3 格 @(58,44) 格26px 间隔2
    for (int i = 0; i < ROOM_CELLS; i++) {
        s_room[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_room[i], 58 + (i % 3) * 28, 44 + (i / 3) * 28);
        lv_obj_set_size(s_room[i], 26, 26);
        lv_obj_set_style_radius(s_room[i], 0, 0);
        lv_obj_set_style_border_width(s_room[i], 1, 0);
        lv_obj_set_style_border_color(s_room[i], lv_color_hex(0x33302C), 0);
        lv_obj_set_style_bg_color(s_room[i], lv_color_hex(COL_BG), 0);
        lv_obj_set_style_pad_all(s_room[i], 0, 0);
    }

    // 血条 ×2 @ y=46/78(外框198×10,内填1..)
    for (int i = 0; i < 2; i++) {
        s_hp[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_hp[i], 0, 46 + i * 32);
        lv_obj_set_size(s_hp[i], 198, 10);
        lv_obj_set_style_radius(s_hp[i], 0, 0);
        lv_obj_set_style_bg_color(s_hp[i], lv_color_hex(COL_BG), 0);
        lv_obj_set_style_border_color(s_hp[i], lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_border_width(s_hp[i], 1, 0);
        lv_obj_set_style_pad_all(s_hp[i], 0, 0);
        s_hpfill[i] = lv_obj_create(s_hp[i]);
        lv_obj_set_pos(s_hpfill[i], 1, 1);
        lv_obj_set_size(s_hpfill[i], 0, 8);
        lv_obj_set_style_radius(s_hpfill[i], 0, 0);
        lv_obj_set_style_bg_color(s_hpfill[i],
                                  lv_color_hex(i == 0 ? COL_ENEMY : COL_ME), 0);
        lv_obj_set_style_border_width(s_hpfill[i], 0, 0);
        lv_obj_set_style_pad_all(s_hpfill[i], 0, 0);
    }

    lv_screen_load(s_scr);
}

// ===================================================================
// 渲染辅助
// ===================================================================
static void render_topbar(const char *title) {
    lv_label_set_text(s_topbar, title);
}

// 页签渲染:active=当前页(金色下划线),nav=光标所在 tab(-1=不在页签区)。
// 焦点(完整金框)与所在页(下划线)是两个独立状态,可同时落在同一 tab。
static void render_tabs(int active, bool village_ok, bool wild_ok, int nav) {
    // 第二页签名随定居进度变化(原版:森林 → 村庄)
    lv_label_set_text(s_tablbl[1], outside_name());
    for (int i = 0; i < 4; i++) {
        lv_obj_clear_flag(s_tabs[i], LV_OBJ_FLAG_HIDDEN);
        bool on = (i == active);
        bool cur = (i == nav);
        bool lock = (i == 1 && !village_ok) || (i == 2 && !wild_ok);
        lv_obj_set_style_text_color(s_tablbl[i],
            lv_color_hex(lock ? COL_LOCK : (on || cur) ? COL_GOLD : COL_DIM), 0);
        lv_obj_set_style_border_color(s_tabs[i],
            lv_color_hex((cur && lock) ? COL_LOCK
                         : (on || cur) ? COL_GOLD : 0x3A3A44), 0);
        lv_obj_set_style_border_width(s_tabs[i],
            on ? 2 : (cur ? 2 : 1), 0);
        // tab 光标保留完整边框(选项行不用框);锁定项光标用锁灰
        lv_obj_set_style_outline_color(s_tabs[i],
            lv_color_hex((cur && lock) ? COL_LOCK : COL_GOLD), 0);
        lv_obj_set_style_outline_width(s_tabs[i], cur ? 1 : 0, 0);
        lv_obj_set_style_outline_pad(s_tabs[i], 0, 0);
    }
}

// 资源格基准排版:46×15 f12。各页显示前必须经此或显式重写,
// 抵消其他页(战斗页拉通 198 宽)留下的几何残留。
static void cell_base(int i, int x, int y) {
    lv_obj_set_pos(s_cells[i], x, y);
    lv_obj_set_size(s_cells[i], 46, 15);
    lv_obj_set_style_text_font(s_cells[i], &dr_font_12, 0);
}

static void render_cells(void) {
    for (int i = 0; i < 8; i++) {
        cell_base(i, (i % 4) * 49, 48 + (i / 4) * 15);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(s_cells[0], "火%s", fire_char());
    lv_label_set_text_fmt(s_cells[1], "人 %u", s.game.population);
    lv_label_set_text_fmt(s_cells[2], "阱 %u", s.game.building_lv[DR_BLD_TRAP]);
    lv_label_set_text_fmt(s_cells[3], "木 %lu", (unsigned long)s.game.res[DR_RES_WOOD]);
    lv_label_set_text_fmt(s_cells[4], "毛 %lu", (unsigned long)s.game.res[DR_RES_FUR]);
    lv_label_set_text_fmt(s_cells[5], "肉 %lu", (unsigned long)s.game.res[DR_RES_MEAT]);
    lv_label_set_text_fmt(s_cells[6], "诱 %lu", (unsigned long)s.game.res[DR_RES_BAIT]);
    lv_label_set_text_fmt(s_cells[7], "革 %lu", (unsigned long)s.game.res[DR_RES_LEATHER]);
}

// 主页日志:逐行标签,最新在上,旧条目颜色渐隐到背景(web 版 fade)。
// 行数随动作行数自适应:3 个动作时更多行,4 个及以上时减少一行。
static lv_obj_t *s_loglines[LOG_VIS];

static lv_color_t log_fade(int i, int n) {
    // i=0 最新:全亮 COL_TEXT;越旧越接近 COL_BG,最深 ~67% 融入背景
    int t = (n <= 1) ? 0 : (i * 170) / (n - 1);
    uint32_t r = ((COL_TEXT >> 16) & 0xFF) * (255 - t) / 255 +
                 ((COL_BG >> 16) & 0xFF) * t / 255;
    uint32_t g = ((COL_TEXT >> 8) & 0xFF) * (255 - t) / 255 +
                 ((COL_BG >> 8) & 0xFF) * t / 255;
    uint32_t b = (COL_TEXT & 0xFF) * (255 - t) / 255 +
                 (COL_BG & 0xFF) * t / 255;
    return lv_color_hex((r << 16) | (g << 8) | b);
}

// 取第 k 新的条目(k=0 最新);没有则返回 NULL
static const char *log_entry_newest_first(int k) {
    int idx = s_log_cnt - 1 - k;
    return (idx >= 0) ? s_logs[idx] : NULL;
}

// 主页日志渲染:n_acts = 本页动作行数(3 或 4)
static void render_home_log(int n_acts) {
    const int pitch = 15;                     // 设计网格行距(90px 恰好 6 行)
    int avail = 184 - 90;
    int max_lines = avail / pitch;
    int show = (n_acts >= 4) ? max_lines - 1 : max_lines;
    if (show > LOG_VIS) show = LOG_VIS;
    for (int i = 0; i < LOG_VIS; i++) {
        lv_obj_t *l = s_loglines[i];
        if (i >= show) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); continue; }
        const char *e = log_entry_newest_first(i);
        if (!e) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_pos(l, 0, 90 + i * pitch);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(l, e);
        lv_obj_set_style_text_color(l, log_fade(i, show), 0);
    }
}

// 动作行:文字 + 焦点 + 冷却填充百分比(0=就绪无底色)
static void set_act(actrow_t *a, const char *text, bool focus, int fill_pct) {
    lv_label_set_text(a->arrow, focus ? ">" : "");
    lv_label_set_text(a->lbl, text);
    lv_obj_set_style_text_color(a->lbl,
        lv_color_hex(focus ? COL_GOLD : COL_TEXT), 0);
    if (fill_pct > 0) {
        lv_obj_clear_flag(a->fill, LV_OBJ_FLAG_HIDDEN);
        // 铺满所在行内宽(主页动作行 198 / 弹窗选项行 172),
        // 剩余时间越少,暗条右端越往左收 —— 从最右端开始减
        int row_w = lv_obj_get_width(a->row);
        lv_obj_set_size(a->fill, (row_w - 2) * fill_pct / 100, 20);
    } else {
        lv_obj_add_flag(a->fill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_acts(void) {
    for (int i = 0; i < ACT_ROWS; i++) lv_obj_add_flag(s_acts[i].row, LV_OBJ_FLAG_HIDDEN);
}

// 列表行:t 名称(focus 金色/lock 灰) v 值(右对齐)
static void set_row(int i, int y, const char *t, const char *v,
                    bool focus, bool lock) {
    listrow_t *r = &s_list[i];
    lv_obj_set_pos(r->row, 0, y);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(r->mark, focus ? ">" : "");
    lv_label_set_text(r->t, t);
    lv_obj_set_style_text_color(r->t,
        lv_color_hex(focus ? COL_GOLD : lock ? COL_LOCK : COL_TEXT), 0);
    lv_label_set_text(r->v, v);
    lv_obj_set_style_text_color(r->v, lv_color_hex(lock ? COL_LOCK : COL_DIM), 0);
}

static void hide_rows(void) {
    for (int i = 0; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
}

// 每页渲染前统一藏起全部场景件,再由各页按需显示
static void clear_scene(void) {
    hide_acts();
    hide_rows();
    for (int i = 0; i < 4; i++) lv_obj_add_flag(s_tabs[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_ok, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAP_CELLS; i++) lv_obj_add_flag(s_map[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) lv_obj_add_flag(s_legend[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < ROOM_CELLS; i++) lv_obj_add_flag(s_room[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) lv_obj_add_flag(s_hp[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
}

// ===================================================================
// 页面渲染
// ===================================================================
static int home_lines(void) {
    int n = 2;   // 点火/添柴 + 建造…
    if (s.game.building_lv[DR_BLD_WORKSHOP] > 0) n++;   // 制造…
    if (s.game.building_lv[DR_BLD_TRADE_POST] > 0) n++; // 贸易…
    return n;
}

// 添柴/点火冷却(原版 Room._STOKE_COOLDOWN,点火添柴共用)
static int stoke_pct(uint32_t now_ms) {
    int32_t rem = (int32_t)(s.rules_rt.stoke_ready_ms - now_ms);
    if (rem <= 0) return 0;
    return (int)(rem * 100 / (DR_STOKE_COOLDOWN_S * 1000u));
}

static void render_home(uint32_t now_ms) {
    render_topbar("小黑屋");
    render_tabs(0, forest_open(), nav_tab_enabled(2), s.nav_focus);
    render_cells();
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    int n = home_lines();
    render_home_log(n);   // 行数随动作行数自适应(3 行动作=更多日志行)
    for (int i = 0; i < n; i++) {
        lv_obj_set_size(s_acts[i].row, ROW_W, ROW_H);
        lv_obj_set_width(s_acts[i].lbl, 170);
        lv_obj_set_style_text_align(s_acts[i].lbl, LV_TEXT_ALIGN_LEFT, 0);
    }
    int idx = 0;
    int gy = 184;
    bool act_focus = (s.nav_focus < 0);   // 光标在页签上时动作区整体去焦点
    bool no_wood = s.game.res[DR_RES_WOOD] < DR_FIRE_LIGHT_COST;
    int sp = stoke_pct(now_ms);
    const char *stoke = (s.game.fire_lv == DR_FIRE_DEAD)
                            ? (sp > 0 ? "点火中…" : (no_wood ? "点火(缺木)" : "点火(5木)"))
                            : (sp > 0 ? "添柴中…" : "添柴(1木)");
    set_act(&s_acts[idx], stoke, act_focus && s.focus == idx, sp);
    lv_obj_set_pos(s_acts[idx].row, 0, gy);
    gy += 24;
    idx++;
    set_act(&s_acts[idx], "建造…", act_focus && s.focus == idx, 0);
    lv_obj_set_pos(s_acts[idx].row, 0, gy);
    gy += 24;
    idx++;
    if (s.game.building_lv[DR_BLD_WORKSHOP] > 0) {
        set_act(&s_acts[idx], "制造…", act_focus && s.focus == idx, 0);
        lv_obj_set_pos(s_acts[idx].row, 0, gy);
        gy += 24;
        idx++;
    }
    if (s.game.building_lv[DR_BLD_TRADE_POST] > 0) {
        set_act(&s_acts[idx], "贸易…", act_focus && s.focus == idx, 0);
        lv_obj_set_pos(s_acts[idx].row, 0, gy);
        idx++;
    }
    for (int i = 0; i < n; i++) lv_obj_clear_flag(s_acts[i].row, LV_OBJ_FLAG_HIDDEN);
}

// 造价组件取值(建造页展示用;组件表见 render_build)
static uint32_t bld_comp_cost(uint8_t res, const dr_bld_cost_t *c) {
    switch (res) {
        case DR_RES_FUR:      return c->fur;
        case DR_RES_MEAT:     return c->meat;
        case DR_RES_IRON:     return c->iron;
        case DR_RES_COAL:     return c->coal;
        case DR_RES_STEEL:    return c->steel;
        case DR_RES_SULPHUR:  return c->sulphur;
        default:              return 0;
    }
}

static void render_build(void) {
    render_topbar("建造");
    // 资源摘要单行 @28:木/毛/肉/诱
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 28);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "木 %lu", (unsigned long)s.game.res[DR_RES_WOOD]);
    lv_label_set_text_fmt(s_cells[1], "毛 %lu", (unsigned long)s.game.res[DR_RES_FUR]);
    lv_label_set_text_fmt(s_cells[2], "肉 %lu", (unsigned long)s.game.res[DR_RES_MEAT]);
    lv_label_set_text_fmt(s_cells[3], "诱 %lu", (unsigned long)s.game.res[DR_RES_BAIT]);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    // 列表 8 建筑 + 返回,行距 23 从 52 起
    for (int i = 0; i < DR_BLD_KIND_COUNT; i++) {
        static const struct { uint8_t res; const char *label; } k_comps[] = {
            { DR_RES_FUR, "毛" },   { DR_RES_MEAT, "肉" },
            { DR_RES_IRON, "铁" },  { DR_RES_COAL, "煤" },
            { DR_RES_STEEL, "钢" }, { DR_RES_SULPHUR, "硫" },
        };
        uint8_t lv = s.game.building_lv[i];
        bool can = dr_rules_can_build(&s.game, i);
        bool warm = s.game.temp_lv > DR_TEMP_COLD;
        char t[24], v[56];
        dr_bld_cost_t c = dr_building_cost(i, lv);
        snprintf(t, sizeof(t), "%s Lv%u", bld_short(i), lv);
        if (c.wood == 0xFFFFFFFFu) {
            snprintf(v, sizeof(v), "上限");            // 满级:不再显示天文缺口
        } else if (can && build_affordable((uint8_t)i) && warm) {
            int n = snprintf(v, sizeof(v), "%lu木", (unsigned long)c.wood);
            for (int k = 0; k < 6 && n > 0 && n < (int)sizeof(v); k++) {
                uint32_t cv = bld_comp_cost(k_comps[k].res, &c);
                if (cv) n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                      (unsigned long)cv, k_comps[k].label);
            }
        } else {
            // 不可选:列缺口;无缺口则说明是室温或解锁问题
            int n = 0;
            uint32_t dw = (c.wood > s.game.res[DR_RES_WOOD])
                              ? c.wood - s.game.res[DR_RES_WOOD] : 0;
            if (dw || c.fur || c.meat || c.iron || c.coal || c.steel || c.sulphur)
                n = snprintf(v, sizeof(v), "缺");
            if (dw) n += snprintf(v + n, sizeof(v) - n, "%lu木", (unsigned long)dw);
            for (int k = 0; k < 6 && n > 0 && n < (int)sizeof(v); k++) {
                uint32_t cv = bld_comp_cost(k_comps[k].res, &c);
                if (!cv) continue;
                uint32_t have = s.game.res[k_comps[k].res];
                if (cv > have)
                    n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                  (unsigned long)(cv - have), k_comps[k].label);
            }
            if (n == 0) snprintf(v, sizeof(v), "%s", warm ? "未解锁" : "太冷");
        }
        set_row(i, 44 + i * 21, t, v, s.focus == i,
                !(can && build_affordable((uint8_t)i) && warm));
    }
    set_row(DR_BLD_KIND_COUNT, 44 + DR_BLD_KIND_COUNT * 21, "返回", "",
            s.focus == DR_BLD_KIND_COUNT, false);
    // 底部说明:光标所选建筑的作用
    static const char *bld_desc[] = {
        "采集木材 +50",            // 板车(原版 carry more wood)
        "每 90s 收获猎物,可叠 10", // 陷阱
        "人口上限+4,流浪者入住",   // 小屋
        "解锁猎人与捕兽人",         // 猎人小屋
        "解锁游牧商人(只买不卖)",  // 贸易站
        "解锁制革匠:毛皮变皮革",    // 制革坊
        "解锁熏肉匠:肉变干肉",      // 熏肉房
        "解锁炼钢工:铁+煤炼钢",     // 炼钢厂
        "解锁军械工:钢+硫造子弹",   // 军械库
    };
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 258);
    if (s.game.temp_lv <= DR_TEMP_COLD)
        lv_label_set_text(s_hint, "屋里太冷,先点火再建造");   // 施工门槛(原版)
    else
        lv_label_set_text(s_hint,
            s.focus < DR_BLD_KIND_COUNT ? bld_desc[s.focus] : "");
}

// 村庄页职业区行号(3..7)→ 职业枚举(含滚动偏移;-1=窗口外)
static int village_row_job(int row) {
    if (row < 3 || row > 7) return -1;
    int job = (row - 3) + s.job_scroll;
    return (job >= 0 && job < DR_JOB_KIND_COUNT) ? job : -1;
}

static void render_village(void) {
    render_topbar(outside_name());
    render_tabs(1, true, nav_tab_enabled(2), -1);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    char v[32];
    bool adj = (s.village_adj >= 0);

    // 行0/1:采集木材、查看陷阱(原版 Outside 面板的两个按钮,60s/90s 冷却)
    int32_t g_rem = (int32_t)(s.rules_rt.gather_ready_ms - now_ms);
    snprintf(v, sizeof(v), g_rem > 0 ? "%lds" : "就绪",
             g_rem > 0 ? (long)((g_rem + 999) / 1000) : 0);
    set_row(0, 52, "收集木材", v, s.focus == 0, false);
    if (s.game.building_lv[DR_BLD_TRAP] > 0) {
        int32_t t_rem = (int32_t)(s.rules_rt.trap_next_ms - now_ms);
        snprintf(v, sizeof(v), t_rem > 0 ? "%lds" : "就绪!",
                 t_rem > 0 ? (long)((t_rem + 999) / 1000) : 0);
        set_row(1, 75, "查看陷阱", v, s.focus == 1, false);
    } else {
        set_row(1, 75, "查看陷阱", "未建", s.focus == 1, true);
    }

    // 行2:采集者 = 未分配人口(自动 +1 木,原版 gatherer,不可调)
    snprintf(v, sizeof(v), "%u人 +1木", dr_rules_job_idle(&s.game));
    set_row(2, 98, "采集者", v, s.focus == 2, true);

    // 行3..7:职业窗口(9 职业,滚动 5 行可见;行内只显示人数,配方在提示行)
    static const char *job_names[DR_JOB_KIND_COUNT] = {
        "猎人", "捕兽人", "制革匠", "熏肉匠", "铁矿工",
        "煤矿工", "硫磺矿工", "炼钢工", "军械工",
    };
    static const char *job_desc[DR_JOB_KIND_COUNT] = {
        "猎人:每 10s 每人 +半张毛皮 +半块肉",
        "捕兽人:每 10s 每人 1 肉换 1 饵",
        "制革匠:每 10s 每人 5 毛皮换 1 皮革",
        "熏肉匠:每 10s 每人 5 肉 + 5 木熏 1 干肉",
        "铁矿工:每 10s 每人吃 1 干肉产 1 铁",
        "煤矿工:每 10s 每人吃 1 干肉产 1 煤",
        "硫磺矿工:每 10s 每人吃 1 干肉产 1 硫",
        "炼钢工:每 10s 每人 1 铁 + 1 煤炼 1 钢",
        "军械工:每 10s 每人 1 钢 + 1 硫造 1 子弹",
    };
    static const char *job_need[DR_JOB_KIND_COUNT] = {
        "猎人小屋", "猎人小屋", "制革坊", "熏肉房",
        "到访铁矿", "到访煤矿", "到访硫磺矿", "炼钢厂", "军械库",
    };
    for (int r = 3; r <= 7; r++) {
        int job = village_row_job(r);
        if (job < 0) { set_row(r, 121 + (r - 3) * 23, "…", "", false, true); continue; }
        bool ok = dr_rules_job_unlocked(&s.game, (uint8_t)job);
        if (ok) snprintf(v, sizeof(v), "%u人", s.game.job[job]);
        else    snprintf(v, sizeof(v), "需%s", job_need[job]);
        set_row(r, 121 + (r - 3) * 23, job_names[job], v,
                s.focus == r, !ok);
    }
    set_row(8, 236, adj ? "完成调节" : "返回", "", s.focus == 8, false);

    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 238);
    if (adj) {
        lv_label_set_text(s_hint, "上加 下减(长按=5) 确定=完成");
    } else if (s.focus >= 3 && s.focus <= 7 &&
               village_row_job(s.focus) >= 0 &&
               dr_rules_job_unlocked(&s.game,
                                     (uint8_t)village_row_job(s.focus))) {
        // 光标停在职业行:显示该职业的产出配方(同建造页提示模式)
        lv_label_set_text(s_hint, job_desc[village_row_job(s.focus)]);
    } else {
        // 概览:人口/上限 + 陷阱副产物与干肉(资源格放不下的那几样)
        char hbuf[96];
        int n = snprintf(hbuf, sizeof(hbuf), "共%u人/上限%u",
                         s.game.population, dr_rules_pop_cap(&s.game));
        if (s.game.res[DR_RES_SCALES] || s.game.res[DR_RES_TEETH] ||
            s.game.res[DR_RES_CLOTH] || s.game.res[DR_RES_FOOD]) {
            n += snprintf(hbuf + n, sizeof(hbuf) - n, " 鳞%lu 牙%lu 布%lu 干%lu",
                          (unsigned long)s.game.res[DR_RES_SCALES],
                          (unsigned long)s.game.res[DR_RES_TEETH],
                          (unsigned long)s.game.res[DR_RES_CLOTH],
                          (unsigned long)s.game.res[DR_RES_FOOD]);
        }
        lv_label_set_text(s_hint, hbuf);
    }
}

// 地形/地标配色(荒野视口)
static uint32_t map_tile_color(uint8_t t, bool seen) {
    if (!seen) return COL_BG;                 // 迷雾
    switch ((dr_world_tile_t)t) {
        case DR_WT_VILLAGE:   return COL_GOLD;
        case DR_WT_FOREST:    return 0x3B5323;
        case DR_WT_FIELD:     return 0x77804A;
        case DR_WT_BARREN:    return 0x6E5B3F;
        case DR_WT_IRON:      return 0xAFAFBC;
        case DR_WT_COAL:      return 0x40404A;
        case DR_WT_SULPHUR:   return 0xC9C25B;
        case DR_WT_OUTPOST:   return 0x5BC8C8;
        case DR_WT_SHIP:      return 0xC85BC8;
        case DR_WT_HOUSE:     return 0xC9A06B;
        case DR_WT_CAVE:      return 0x6B7FC9;
        case DR_WT_TOWN:      return 0xC96B8E;
        case DR_WT_CITY:      return 0x8E6BC9;
        default:              return COL_BG;
    }
}

static const char *map_tile_name(uint8_t t) {
    switch ((dr_world_tile_t)t) {
        case DR_WT_VILLAGE:  return "小屋";
        case DR_WT_FOREST:   return "森林";
        case DR_WT_FIELD:    return "田野";
        case DR_WT_BARREN:   return "荒地";
        case DR_WT_IRON:     return "铁矿";
        case DR_WT_COAL:     return "煤矿";
        case DR_WT_SULPHUR:  return "硫磺矿";
        case DR_WT_OUTPOST:  return "哨站";
        case DR_WT_SHIP:     return "星舰";
        case DR_WT_HOUSE:    return "老屋";
        case DR_WT_CAVE:     return "洞穴";
        case DR_WT_TOWN:     return "废镇";
        case DR_WT_CITY:     return "城市";
        default:             return "?";
    }
}

static void render_map(void) {
    render_topbar("荒野");
    render_tabs(2, forest_open(), true, -1);

    if (!s.game.in_wilderness) {
        // 出发整备(原版 Path 承重制):干肉/药/子弹在重量预算内自选
        for (int i = 0; i < 4; i++) {
            cell_base(i, i * 49, 24);
            lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_cells[0], "水 %u", dr_world_water_cap(&s.game));
        lv_label_set_text_fmt(s_cells[1], "重 %u.%u/10",
                              dr_world_bag_weight(&s.game, &s_world) / 10u,
                              dr_world_bag_weight(&s.game, &s_world) % 10u);
        lv_label_set_text_fmt(s_cells[2], "HP %u", dr_world_health_cap(&s.game));
        lv_label_set_text_fmt(s_cells[3], "行 %u", 0);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        char v[24];
        bool adj = (s.outfit_adj >= 0);
        snprintf(v, sizeof(v), "库%lu 带%u", (unsigned long)s.game.res[DR_RES_FOOD],
                 s.game.food);
        set_row(0, 100, "干肉", v, s.focus == 0, false);
        snprintf(v, sizeof(v), "库%lu 带%u",
                 (unsigned long)s.game.res[DR_RES_MEDICINE],
                 dr_world_carry_medicine(&s_world));
        set_row(1, 124, "药", v, s.focus == 1,
                s.game.res[DR_RES_MEDICINE] == 0 &&
                    dr_world_carry_medicine(&s_world) == 0);
        snprintf(v, sizeof(v), "库%lu 带%u",
                 (unsigned long)s.game.res[DR_RES_BULLETS],
                 dr_world_carry_bullets(&s_world));
        set_row(2, 148, "子弹", v, s.focus == 2,
                s.game.res[DR_RES_BULLETS] == 0 &&
                    dr_world_carry_bullets(&s_world) == 0);
        if ((int32_t)(now_ms - s.embark_cd_ms) < 0)
            snprintf(v, sizeof(v), "%lds",
                     (long)((s.embark_cd_ms - now_ms + 999) / 1000));
        else
            snprintf(v, sizeof(v), "就绪");
        set_row(3, 176, "出发远征", v, s.focus == 3, s.game.food == 0);
        set_row(4, 200, "返回", "", s.focus == 4, false);
        for (int i = 5; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hint, 0, 228);
        if (adj)
            lv_label_set_text(s_hint, "上加1 下减1(长按=5) 确定=下一项");
        else if (s.game.food == 0)
            lv_label_set_text(s_hint, "整备干肉后才能出发(库→带)");
        else
            lv_label_set_text(s_hint, "背袋承重10:干肉/药各占1,子弹10发占1");
        return;
    }

    // 远征中:9×9 视口(中心=主角)+ 方向行
    for (int i = 0; i < MAP_CELLS; i++) {
        lv_obj_clear_flag(s_map[i], LV_OBJ_FLAG_HIDDEN);
        int cx = (int)s.game.hero_x + (i % 9) - 4;
        int cy = (int)s.game.hero_y + (i / 9) - 4;
        bool me = (i % 9 == 4 && i / 9 == 4);
        uint8_t t = dr_world_tile(&s_world, cx, cy);
        bool seen = me || dr_world_seen(&s_world, cx, cy);
        lv_obj_set_style_bg_color(s_map[i],
            lv_color_hex(me ? COL_ME : map_tile_color(t, seen)), 0);
    }
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "水 %u", s.game.water);
    lv_label_set_text_fmt(s_cells[1], "食 %u", s.game.food);
    lv_label_set_text_fmt(s_cells[2], "HP %u/%u", s.game.hero_hp, s.game.hero_hp_max);
    lv_label_set_text_fmt(s_cells[3], "里 %u", dr_world_home_dist(&s.game));

    // 方向行:显示目标格(雾内显示"未知")
    static const int dirs[4][2] = { {1,0},{0,1},{-1,0},{0,-1} };
    static const char *dir_names[4] = { "东", "南", "西", "北" };
    char v[24];
    for (int d = 0; d < 4; d++) {
        int tx = (int)s.game.hero_x + dirs[d][0];
        int ty = (int)s.game.hero_y + dirs[d][1];
        if (tx < 0 || ty < 0 || tx >= DR_WORLD_SIZE || ty >= DR_WORLD_SIZE)
            snprintf(v, sizeof(v), "尽头");
        else if (dr_world_seen(&s_world, tx, ty))
            snprintf(v, sizeof(v), "%s", map_tile_name(dr_world_tile(&s_world, tx, ty)));
        else
            snprintf(v, sizeof(v), "未知");
        set_row(d, 158 + d * 24, dir_names[d], v, s.focus == d, false);
    }
    snprintf(v, sizeof(v), "余%u", s.game.food);
    set_row(4, 158 + 4 * 24, "吃干肉", s.game.food ? v : "没有",
            s.focus == 4, s.game.food == 0);
    for (int i = 5; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 285);
    lv_label_set_text(s_hint, dr_world_danger(&s.game)
                                  ? "离小屋太远,没有护甲很危险"
                                  : "回到小屋格即安全到家");
}

static void render_ruin(void) {
    render_topbar(dr_world_location_name(dr_world_location(&s_world)));
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "水 %u", s.game.water);
    lv_label_set_text_fmt(s_cells[1], "食 %u", s.game.food);
    lv_label_set_text_fmt(s_cells[2], "HP %u", s.game.hero_hp);
    lv_label_set_text_fmt(s_cells[3], "火把 %s",
        (s.game.flags & ((uint64_t)1u << DR_FLAG_TORCH)) ? "有" : "无");
    for (int i = 0; i < ROOM_CELLS; i++)
        lv_obj_add_flag(s_room[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_legend[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_legend[1], LV_OBJ_FLAG_HIDDEN);
    bool has_torch =
        (s.game.flags & ((uint64_t)1u << DR_FLAG_TORCH)) != 0;
    set_row(0, 158, "搜索", "再掷一次", s.focus == 0, false);
    set_row(1, 182, "离开地点", "回荒野", s.focus == 1, false);
    for (int i = 2; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 215);
    uint8_t loc = dr_world_location(&s_world);
    lv_label_set_text(s_hint,
        loc == DR_WT_CAVE
            ? (has_torch ? "洞穴幽深;火把只够一次深入" : "需要火把(工坊:1木1布)")
        : loc == DR_WT_HOUSE ? "老屋:也许有药,也许有埋伏"
        : loc == DR_WT_TOWN  ? "废镇:学校与医院,或街头伏击"
        : loc == DR_WT_CITY  ? "废墟城市:空楼,士兵,或值钱的物资"
                              : "");
}

static const char *weapon_name(uint8_t lv) {
    static const char *n[5] = { "拳", "骨矛", "铁剑", "钢剑", "步枪" };
    return (lv < 5) ? n[lv] : n[0];
}

static void render_combat(void) {
    render_topbar("战斗");
    uint8_t e = dr_world_fight_enemy(&s_world, &s.game);
    // 敌我名行(198 通栏)+ 血条(名30/条46/名62/条78)
    for (int i = 2; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_base(0, 0, 30);
    cell_base(1, 0, 62);
    lv_obj_set_size(s_cells[0], 198, 15);
    lv_obj_set_size(s_cells[1], 198, 15);
    lv_obj_clear_flag(s_cells[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cells[1], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "%s HP %u/%u",
                          dr_enemy_name(e),
                          (unsigned)dr_world_fight_hp(&s_world),
                          (unsigned)dr_world_fight_hp_max(&s_world));
    lv_label_set_text_fmt(s_cells[1], "你(%s) HP %u/%u",
                          weapon_name(s.game.weapon_lv),
                          s.game.hero_hp, s.game.hero_hp_max);
    uint16_t ehp = dr_world_fight_hp(&s_world), ehp_max = dr_world_fight_hp_max(&s_world);
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_flag(s_hp[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hp[i], 0, 46 + i * 32);
        int w = (i == 0)
            ? (ehp_max ? (int)(198u * ehp / ehp_max) : 0)
            : (int)(198u * s.game.hero_hp / (s.game.hero_hp_max ? s.game.hero_hp_max : 1));
        lv_obj_set_size(s_hpfill[i], w, 8);
    }
    lv_obj_add_flag(s_loglines[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_loglines[1], LV_OBJ_FLAG_HIDDEN);
    char a[24];
    snprintf(a, sizeof(a), "攻击(%s)", weapon_name(s.game.weapon_lv));
    set_act(&s_acts[0], a, s.focus == 0, 0);
    snprintf(a, sizeof(a), "吃干肉(余%u)", s.game.food);
    set_act(&s_acts[1], a, s.focus == 1, 0);
    snprintf(a, sizeof(a), "用药(余%u)", dr_world_carry_medicine(&s_world));
    set_act(&s_acts[2], a, s.focus == 2, 0);
    set_act(&s_acts[3], "逃跑", s.focus == 3, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_pos(s_acts[i].row, 0, 168 + i * 24);
        lv_obj_clear_flag(s_acts[i].row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 270);
    lv_label_set_text(s_hint, "命中80%;敌按攻击间隔反击");
}

static void render_trade(void) {
    render_topbar("贸易");
    // 支付货币摘要:毛/鳞/牙(原版 TradeGoods 以此三者支付,只买不卖)
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "木 %lu", (unsigned long)s.game.res[DR_RES_WOOD]);
    lv_label_set_text_fmt(s_cells[1], "毛 %lu", (unsigned long)s.game.res[DR_RES_FUR]);
    lv_label_set_text_fmt(s_cells[2], "鳞 %lu", (unsigned long)s.game.res[DR_RES_SCALES]);
    lv_label_set_text_fmt(s_cells[3], "牙 %lu", (unsigned long)s.game.res[DR_RES_TEETH]);
    bool post = s.game.building_lv[DR_BLD_TRADE_POST] > 0;
    bool compass = (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) != 0;
    char v[28];
    if (post) {
        snprintf(v, sizeof(v), "%u毛", 150u);  set_row(0, 46, "买 鳞", v, s.focus == 0, !trade_affordable(0));
        snprintf(v, sizeof(v), "%u毛", 300u);  set_row(1, 67, "买 牙", v, s.focus == 1, !trade_affordable(1));
        snprintf(v, sizeof(v), "%u毛%u鳞", 150u, 50u); set_row(2, 88, "买 铁", v, s.focus == 2, !trade_affordable(2));
        snprintf(v, sizeof(v), "%u毛%u牙", 200u, 50u); set_row(3, 109, "买 煤", v, s.focus == 3, !trade_affordable(3));
        snprintf(v, sizeof(v), "%u毛%u鳞%u牙", 300u, 50u, 50u); set_row(4, 130, "买 钢", v, s.focus == 4, !trade_affordable(4));
        snprintf(v, sizeof(v), "%u鳞", 10u);   set_row(5, 151, "买 子弹", v, s.focus == 5, !trade_affordable(5));
        snprintf(v, sizeof(v), "%u鳞%u牙", 50u, 30u); set_row(6, 172, "买 药", v, s.focus == 6, !trade_affordable(6));
        if (compass) snprintf(v, sizeof(v), "已购");
        else         snprintf(v, sizeof(v), "%u毛%u鳞%u牙", 400u, 20u, 10u);
        set_row(7, 193, "买 罗盘", v, s.focus == 7, !trade_affordable(7));
        set_row(8, 215, "返回", "", s.focus == 8, false);
    } else {
        for (int i = 0; i < 8; i++) set_row(i, 46 + i * 21, "—", "需贸易站", false, true);
        set_row(8, 215, "返回", "", s.focus == 8, false);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 238);
    lv_label_set_text(s_hint, "游牧商人:以毛皮/鳞/牙易货(M4 远征用品)");
}

static void render_settings(void) {
    render_topbar("设置");
    render_tabs(3, forest_open(), nav_tab_enabled(2), -1);
    set_row(0, 52, "操作说明", "键位:三键", s.focus == 0, false);
    set_row(1, 76, "重开本局", "需确认", s.focus == 1, false);
    set_row(2, 100, "关于", "v0.6", s.focus == 2, false);
    set_row(3, 124, "返回", "", s.focus == 3, false);
}

static void render_event(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    const dr_event_t *e = &ev[s.ev_idx];
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dpanel, 22, 42);   // 水平居中
    lv_obj_set_size(s_dpanel, 196, 236);
    lv_obj_add_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dbody, 0, 0);      // 面板已有 pad10,子坐标不再叠加偏移
    lv_obj_set_size(s_dbody, 176, 80);
    set_dbody(dr_text(e->text_id));
    // 选项底部锚定(mockup ④):最后一行固定 y=164,向上堆叠
    for (int i = 0; i < e->choice_count && i < 4; i++) {
        actrow_t *a = &s_dacts[i];
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(a->row, 0, 164 - (e->choice_count - 1 - i) * 24);
        set_act(a, dr_text(e->choices[i].text_id), s.focus == i, 0);
    }
    for (int i = e->choice_count; i < 4; i++)
        lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
}

static void render_confirm(void) {
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dpanel, 22, 60);   // 水平居中,垂直 (320-200)/2=60
    lv_obj_set_size(s_dpanel, 196, 200);
    lv_obj_clear_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_dtitle, "确认操作");
    lv_obj_clear_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dbody, 0, 42);
    lv_obj_set_size(s_dbody, 176, 40);
    set_dbody("确定重开本局?当前进度将丢失。");
    for (int i = 0; i < 2; i++) {
        actrow_t *a = &s_dacts[i];
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(a->row, 0, 130 + i * 24);
        set_act(a, i == 0 ? "确认执行" : "取消", s.focus == i, 0);
    }
    for (int i = 2; i < 4; i++)
        lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
}

// ---- 制造页(工坊建成后从小屋页进入;原版 crafts,§9.7) ----
static const char *craft_name(uint8_t c) {
    static const char *n[DR_CRAFT_KIND_COUNT] = {
        "火把", "骨矛", "铁剑", "钢剑", "步枪",
        "皮甲", "铁甲", "钢甲",
        "水袋", "木桶", "水箱", "背囊", "篷车", "车队",
    };
    return n[c];
}

static void render_craft(void) {
    render_topbar("制造");
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "木 %lu", (unsigned long)s.game.res[DR_RES_WOOD]);
    lv_label_set_text_fmt(s_cells[1], "革 %lu", (unsigned long)s.game.res[DR_RES_LEATHER]);
    lv_label_set_text_fmt(s_cells[2], "鳞 %lu", (unsigned long)s.game.res[DR_RES_SCALES]);
    lv_label_set_text_fmt(s_cells[3], "铁 %lu", (unsigned long)s.game.res[DR_RES_IRON]);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);

    static const struct { uint8_t res; const char *label; } comps[] = {
        { DR_RES_WOOD, "木" },   { DR_RES_TEETH, "牙" },
        { DR_RES_LEATHER, "革" },{ DR_RES_IRON, "铁" },
        { DR_RES_STEEL, "钢" },  { DR_RES_SULPHUR, "硫" },
        { DR_RES_CLOTH, "布" },
    };
    int row = 0;
    for (int c = 0; c < DR_CRAFT_KIND_COUNT && row < LIST_ROWS - 1; c++) {
        if (!dr_rules_craft_visible(&s.game, (uint8_t)c)) continue;
        char t[20], v[64];
        snprintf(t, sizeof(t), "%s", craft_name((uint8_t)c));
        bool owned = dr_rules_craft_owned(&s.game, (uint8_t)c);
        if (owned && c != DR_CRAFT_TORCH) {
            snprintf(v, sizeof(v), "已有");
        } else {
            int n = 0;
            for (int k = 0; k < 7; k++) {
                uint32_t need = dr_rules_craft_need(&s.game, (uint8_t)c,
                                                    comps[k].res);
                if (!need) continue;
                if (n == 0) n = snprintf(v, sizeof(v), "%lu%s",
                                         (unsigned long)need, comps[k].label);
                else n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                   (unsigned long)need, comps[k].label);
            }
            if (n == 0) snprintf(v, sizeof(v), "免费");
        }
        bool can = dr_rules_craft_ready(&s.game, (uint8_t)c);
        set_row(row, 52 + row * 23, t, v, s.focus == row, !can);
        s.craft_row_map[row] = (uint8_t)c;
        row++;
    }
    s.craft_row_count = (uint8_t)row;
    for (int i = row; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    set_row(row, 52 + row * 23, "返回", "", s.focus == row, false);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 258);
    lv_label_set_text(s_hint, "武器/护甲自动装备取最优;水具/背具即刻生效");
}

static void render_title(void) {
    lv_label_set_text(s_topbar, "");
    lv_label_set_text(s_batt, "");
    lv_obj_clear_flag(s_t_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_ok, LV_OBJ_FLAG_HIDDEN);
}

static void render(void) {
    if (!s.dirty) return;
    s.dirty = false;
    s.last_render_ms = (uint32_t)(esp_timer_get_time() / 1000);
    clear_scene();   // 先藏全部场景件,各页再按需显示(防跨页残留)
    switch (s.page) {
        case PG_TITLE:   render_title(); break;
        case PG_HOME:    render_home((uint32_t)(esp_timer_get_time() / 1000)); break;
        case PG_BUILD:   render_build(); break;
        case PG_VILLAGE: render_village(); break;
        case PG_MAP:     render_map(); break;
        case PG_RUIN:    render_ruin(); break;
        case PG_COMBAT:  render_combat(); break;
        case PG_TRADE:   render_trade(); break;
        case PG_SETTINGS:render_settings(); break;
        case PG_EVENT:   render_event(); break;
        case PG_CONFIRM: render_confirm(); break;
        case PG_CRAFT:   render_craft(); break;
    }
}

// ===================================================================
// 页面切换与动作
// ===================================================================
static int page_lines(void);
static bool row_enabled(int idx);
static void focus_move(int dir);

static int page_lines(void) {
    switch (s.page) {
        case PG_HOME: return home_lines();
        case PG_BUILD: return DR_BLD_KIND_COUNT + 1;
        case PG_VILLAGE: return 9;   // 采集/陷阱/采集者 + 职业窗口5 + 返回
        case PG_CRAFT:   return s.craft_row_count + 1;   // 可见制造项 + 返回
        case PG_MAP: return 5;   // 整备3行+出发+返回 / 或 东南西北+吃干肉
        case PG_RUIN: return 2;    // 搜索 / 离开
        case PG_COMBAT: return 4;
        case PG_TRADE: return 9;
        case PG_SETTINGS: return 4;
        case PG_EVENT: {
            uint16_t c = 0;
            const dr_event_t *ev = dr_events_table(&c);
            return ev[s.ev_idx].choice_count;
        }
        case PG_CONFIRM: return 2;
        default: return 1;
    }
}

// 建造行可用:材料足额付得起(可见性规则之上的硬门槛;不足的行不可选,
// 光标跳过——三键约定"锁定或资源不足的行自动跳过")
static bool build_affordable(uint8_t id) {
    dr_bld_cost_t c = dr_building_cost(id, s.game.building_lv[id]);
    if (c.wood == 0xFFFFFFFFu) return false;
    return s.game.res[DR_RES_WOOD] >= c.wood &&
           s.game.res[DR_RES_FUR] >= c.fur &&
           s.game.res[DR_RES_MEAT] >= c.meat;
}

// 贸易行可用:纯支付能力检查(不执行交易;原版牌价见 dr_rules.c k_trade_cost)
static bool trade_affordable(uint8_t item) {
    if (s.game.building_lv[DR_BLD_TRADE_POST] == 0) return false;
    if (item == DR_TRADE_COMPASS &&
        (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS))) return false;
    switch ((dr_trade_t)item) {
        case DR_TRADE_SCALES: return s.game.res[DR_RES_FUR] >= 150u;
        case DR_TRADE_TEETH:  return s.game.res[DR_RES_FUR] >= 300u;
        case DR_TRADE_IRON:   return s.game.res[DR_RES_FUR] >= 150u &&
                                      s.game.res[DR_RES_SCALES] >= 50u;
        case DR_TRADE_COAL:   return s.game.res[DR_RES_FUR] >= 200u &&
                                      s.game.res[DR_RES_TEETH] >= 50u;
        case DR_TRADE_STEEL:  return s.game.res[DR_RES_FUR] >= 300u &&
                                      s.game.res[DR_RES_SCALES] >= 50u &&
                                      s.game.res[DR_RES_TEETH] >= 50u;
        case DR_TRADE_BULLETS:return s.game.res[DR_RES_SCALES] >= 10u;
        case DR_TRADE_MEDICINE:
            return s.game.res[DR_RES_SCALES] >= 50u &&
                   s.game.res[DR_RES_TEETH] >= 30u;
        case DR_TRADE_COMPASS:return s.game.res[DR_RES_FUR] >= 400u &&
                                      s.game.res[DR_RES_SCALES] >= 20u &&
                                      s.game.res[DR_RES_TEETH] >= 10u;
        default: return false;
    }
}

// 行可用性:禁用行(锁定/资源不足)被光标跳过,只保留视觉灰显说明原因。
static bool row_enabled(int idx) {
    switch (s.page) {
        case PG_BUILD:
            return idx == DR_BLD_KIND_COUNT ||          // 返回
                   (dr_rules_can_build(&s.game, idx) &&
                    build_affordable((uint8_t)idx) &&
                    s.game.temp_lv > DR_TEMP_COLD);     // 过冷也不可选(提示见页底)
        case PG_VILLAGE:
            if (idx == 0 || idx == 8) return true;      // 采集 / 返回
            if (idx == 1) return s.game.building_lv[DR_BLD_TRAP] > 0;
            if (idx == 2) return false;                 // 采集者自动,不可调
            {   // 职业窗口(窗口外恒禁用)
                int job = village_row_job(idx);
                if (job < 0) return false;
                return dr_rules_job_unlocked(&s.game, (uint8_t)job);
            }
        case PG_TRADE:
            return idx == 8 || trade_affordable((uint8_t)idx);
        case PG_MAP:
            if (!s.game.in_wilderness) {
                if (idx == 0 || idx == 4) return true;         // 干肉 / 返回
                if (idx == 1) return s.game.res[DR_RES_MEDICINE] > 0 ||
                                   dr_world_carry_medicine(&s_world) > 0;
                if (idx == 2) return s.game.res[DR_RES_BULLETS] > 0 ||
                                   dr_world_carry_bullets(&s_world) > 0;
                // idx 3 = 出发:需已带干肉 + 冷却已过
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                return s.game.food > 0 &&
                       (int32_t)(now_ms - s.embark_cd_ms) >= 0;
            }
            if (idx == 4) return s.game.food > 0;   // 吃干肉
            return idx < 4;                          // 东南西北
        case PG_CRAFT:
            return idx >= s.craft_row_count ||      // 返回
                   dr_rules_craft_ready(&s.game, s.craft_row_map[idx]);
        default:
            return true;   // 主页动作/设置/地图/弹窗选项均无禁用态
    }
}

// 焦点按 dir(±1)步进,跳过禁用行;整页全禁用则原地不动。
static void focus_move(int dir) {
    int lines = page_lines();
    for (int i = 0; i < lines; i++) {
        s.focus = (s.focus + dir + lines) % lines;
        if (row_enabled(s.focus)) return;
    }
}

static void page_goto(page_t p) {
    s.page = p;
    s.focus = 0;
    s.village_adj = -1;   // 离开村庄页/任意切页退出人数调节
    s.outfit_adj = -1;    // 离开整备态
    // 回到小屋页 = 拜访房间(原版 onArrival):陌生人从沉睡中醒来帮忙
    if (p == PG_HOME && dr_rules_builder_visit(&s.game)) {
        log_push("她站在火边:可以帮忙了");
        s.save_pending = true;
    }
    if (!row_enabled(0)) focus_move(1);   // 首行禁用则落到首个可用行
    s.dirty = true;
}

static void home_action(int idx) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (idx == 0) {                           // 点火/添柴(共用 10s 冷却)
        if ((int32_t)(s.rules_rt.stoke_ready_ms - now_ms) > 0) {
            log_push("火头正旺,先等等再添");
        } else if (dr_rules_stoke_fire(&s.game, dr_port_now_ts())) {
            s.rules_rt.fire_deadline_ms = now_ms + DR_FIRE_LEVEL_SECONDS * 1000u;
            s.rules_rt.stoke_ready_ms = now_ms + DR_STOKE_COOLDOWN_S * 1000u;
            log_push(s.game.fire_lv == DR_FIRE_ROARING ? "火烧得炽烈" : "火烧得更旺了些");
            s.save_pending = true;
        } else {
            log_push(s.game.fire_lv == DR_FIRE_DEAD ? "不够木头把火生起来" : "木柴用完了");
        }
        return;
    }
    if (idx == 1) { page_goto(PG_BUILD); return; }   // 建造…
    if (idx == 2 && s.game.building_lv[DR_BLD_WORKSHOP] > 0) {
        page_goto(PG_CRAFT);                          // 制造…
        return;
    }
    page_goto(PG_TRADE);                              // 贸易…(行存在即已建贸易站)
}

// 村庄页动作(行0/1):采集木材、查看陷阱
static void village_action(int idx) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (idx == 0) {                           // 收集木材(60s;板车 +50)
        if ((int32_t)(s.rules_rt.gather_ready_ms - now_ms) > 0) {
            log_push("还需要等等才能再收集");
        } else {
            dr_rules_gather(&s.game, dr_port_now_ts());
            s.rules_rt.gather_ready_ms = now_ms + DR_GATHER_COOLDOWN_S * 1000u;
            log_push(s.game.building_lv[DR_BLD_CART] > 0 ? "收集到 50 木" : "收集到 10 木");
            s.save_pending = true;
        }
        return;
    }
    if (idx == 1) {                           // 查看陷阱(90s)
        dr_trap_yield_t y;
        if (!dr_rules_trap_check(&s.rules_rt, &s.game, now_ms, &y)) {
            log_push("刚查过陷阱,再等等");
        } else {
            char line[64];
            int n = snprintf(line, sizeof(line), "陷阱收获:");
            struct { const char *name; uint8_t cnt; } parts[6] = {
                {" 毛皮×", y.fur}, {" 肉×", y.meat}, {" 鳞×", y.scales},
                {" 牙×", y.teeth}, {" 布×", y.cloth}, {" 护符×", y.charm},
            };
            for (int i = 0; i < 6 && n > 0 && n < (int)sizeof(line); i++) {
                if (parts[i].cnt)
                    n += snprintf(line + n, sizeof(line) - n, "%s%u",
                                  parts[i].name, parts[i].cnt);
            }
            if (n > 0 && n < (int)sizeof(line) && y.bait)
                snprintf(line + n, sizeof(line) - n, "(耗%u饵)", y.bait);
            log_push(line);
            s.save_pending = true;
        }
        return;
    }
}

static void list_action(int idx) {
    switch (s.page) {
        case PG_BUILD:
            if (idx >= DR_BLD_KIND_COUNT) { page_goto(PG_HOME); return; }
            if (dr_rules_build(&s.game, idx, dr_port_now_ts())) {
                char line[48];
                snprintf(line, sizeof(line), "建成 %s Lv%u", bld_short(idx),
                         s.game.building_lv[idx]);
                log_push(line);
                s.save_pending = true;
            } else if (s.game.temp_lv <= DR_TEMP_COLD) {
                log_push("屋里太冷,先点火再建造");
            } else {
                log_push("材料不够");
            }
            break;
        case PG_VILLAGE:
            if (idx == 7) { page_goto(PG_HOME); break; }
            if (idx == 0 || idx == 1) { village_action(idx); break; }
            if (idx == 2) { log_push("采集者自动拾柴(+1木)"); break; }
            {   // 行3..6:进入人数调节
                int job = village_row_job(idx);
                if (job < 0 || !dr_rules_job_unlocked(&s.game, (uint8_t)job)) {
                    log_push(job == DR_JOB_TANNER ? "需先建制革坊" :
                             job == DR_JOB_CHARCUTIER ? "需先建熏肉房" :
                                                        "需先建猎人小屋");
                } else {
                    s.village_adj = (int8_t)job;   // 进入人数调节
                    s.dirty = true;
                }
            }
            break;
        case PG_MAP: {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (!s.game.in_wilderness) {
                static const uint8_t outfit_res[3] = {
                    DR_RES_FOOD, DR_RES_MEDICINE, DR_RES_BULLETS
                };
                if (idx <= 2) {                     // 进入携带调配
                    s.outfit_adj = (int8_t)idx;
                    s.dirty = true;
                    break;
                }
                if (idx == 3) {                     // 出发远征(需已带干肉)
                    if ((int32_t)(now_ms - s.embark_cd_ms) < 0) {
                        log_push("歇一歇再出发");
                    } else if (dr_world_embark(&s_world, &s.game)) {
                        log_push("踏上尘土路");
                        s.save_pending = true;
                    } else {
                        log_push("先整备干肉(库→带)");
                    }
                    break;
                }
                page_goto(PG_HOME);
                break;
            }
            if (idx <= 3) {                           // 东南西北移动一格
                static const int dirs[4][2] = {
                    {1, 0}, {0, 1}, {-1, 0}, {0, -1}
                };
                uint64_t flags_before = s.game.flags;
                dr_move_result_t r =
                    dr_world_move(&s_world, &s.game, dirs[idx][0], dirs[idx][1]);
                switch (r) {
                    case DR_MOVE_FIGHT: {
                        char line[48];
                        snprintf(line, sizeof(line), "遭遇了%s!",
                                 dr_enemy_name(dr_world_fight_enemy(&s_world,
                                                                    &s.game)));
                        log_push(line);
                        page_goto(PG_COMBAT);
                        break;
                    }
                    case DR_MOVE_WARN_THIRST: log_push("口渴难忍"); break;
                    case DR_MOVE_WARN_HUNGER: log_push("饥饿来袭"); break;
                    case DR_MOVE_DEATH:
                        log_push("你倒在了荒野里,物资全失");
                        s.embark_cd_ms = now_ms + 120u * 1000u;
                        s.save_pending = true;
                        break;
                    case DR_MOVE_HOME: {
                        log_push("回到了小屋");
                        if (!(flags_before & ((uint64_t)1u << DR_FLAG_IRON_MINE)) &&
                            (s.game.flags & ((uint64_t)1u << DR_FLAG_IRON_MINE)))
                            log_push("可以派村民去采铁矿了");
                        if (!(flags_before & ((uint64_t)1u << DR_FLAG_COAL_MINE)) &&
                            (s.game.flags & ((uint64_t)1u << DR_FLAG_COAL_MINE)))
                            log_push("可以派村民去采煤矿了");
                        if (!(flags_before & ((uint64_t)1u << DR_FLAG_SULPHUR_MINE)) &&
                            (s.game.flags & ((uint64_t)1u << DR_FLAG_SULPHUR_MINE)))
                            log_push("可以派村民去采硫磺矿了");
                        s.save_pending = true;
                        page_goto(PG_HOME);
                        break;
                    }
                    case DR_MOVE_OUTPOST:   log_push("哨站:把水囊灌满了"); break;
                    case DR_MOVE_IRON:      log_push("发现铁矿!(回家后可派矿工)"); break;
                    case DR_MOVE_COAL:      log_push("发现煤矿!(回家后可派矿工)"); break;
                    case DR_MOVE_SULPHUR:   log_push("发现硫磺矿!(回家后可派矿工)"); break;
                    case DR_MOVE_SHIP:      log_push("一艘坠毁的星舰躺在荒野上(M5)"); break;
                    case DR_MOVE_HOUSE:
                    case DR_MOVE_CAVE:
                    case DR_MOVE_TOWN:
                    case DR_MOVE_CITY: {
                        char msg[48];
                        snprintf(msg, sizeof(msg), "来到了%s",
                                 dr_world_location_name(dr_world_location(
                                     &s_world)));
                        log_push(msg);
                        page_goto(PG_RUIN);
                        break;
                    }
                    case DR_MOVE_BLOCKED:   log_push("世界的尽头"); break;
                    case DR_MOVE_OK:        s.save_pending = true; break;
                }
                s.dirty = true;
                break;
            }
            if (idx == 4) {                           // 吃干肉(回 8 HP)
                if (dr_world_eat(&s.game)) {
                    log_push("吃了口干肉,缓过劲来");
                    s.save_pending = true;
                } else {
                    log_push("没有干肉了");
                }
                break;
            }
            break;
        }
        case PG_COMBAT: {
            dr_fight_result_t r;
            if (idx == 0)            r = dr_world_fight_attack(&s_world, &s.game);
            else if (idx == 1)       r = dr_world_fight_eat(&s_world, &s.game);
            else if (idx == 2)       r = dr_world_fight_medicine(&s_world, &s.game);
            else                     r = dr_world_fight_flee(&s_world, &s.game);
            const char *en = dr_enemy_name(dr_world_fight_enemy(&s_world, &s.game));
            (void)en;
            switch (r) {
                case DR_FIGHT_WIN:
                    log_push("击败了它!战利品入包");
                    s.save_pending = true;
                    page_goto(PG_MAP);
                    break;
                case DR_FIGHT_LOSE:
                    log_push("你被它打倒了,物资全失");
                    s.embark_cd_ms = (uint32_t)(esp_timer_get_time() / 1000) +
                                     120u * 1000u;
                    s.save_pending = true;
                    page_goto(PG_MAP);
                    break;
                case DR_FIGHT_FLED:
                    log_push("逃掉了");
                    page_goto(PG_MAP);
                    break;
                case DR_FIGHT_NONE:
                    if (idx == 1) log_push("没有干肉了");
                    if (idx == 2) log_push("没有药了");
                    break;
                case DR_FIGHT_MISS:       log_push("挥空了"); break;
                case DR_FIGHT_HIT:        log_push("命中!"); break;
                case DR_FIGHT_ENEMY_HIT:  log_push("它反击得手"); break;
                case DR_FIGHT_ENEMY_MISS: log_push("它扑了个空"); break;
                case DR_FIGHT_ENEMY_SKIP: log_push("它蓄势未发"); break;
                default: break;
            }
            s.dirty = true;
            break;
        }
        case PG_RUIN: {   // 地点页(老屋/洞穴/废镇/城市):搜索/离开
            if (idx == 1 || !dr_world_location(&s_world)) {
                dr_world_location_leave(&s_world);
                page_goto(PG_MAP);
                break;
            }
            char line[64] = {0};
            dr_loc_result_t r = dr_world_location_search(&s_world, &s.game,
                                                         line, sizeof(line));
            switch (r) {
                case DR_LOC_LOOT:
                case DR_LOC_WATER: {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "搜到:%s", line);
                    log_push(msg);
                    s.save_pending = true;
                    break;
                }
                case DR_LOC_FIGHT: {
                    char msg[48];
                    snprintf(msg, sizeof(msg), "%s扑了过来!",
                             dr_enemy_name(dr_world_fight_enemy(&s_world,
                                                                &s.game)));
                    log_push(msg);
                    page_goto(PG_COMBAT);
                    break;
                }
                case DR_LOC_EMPTY:      log_push("这里被搜空了"); break;
                case DR_LOC_NEED_TORCH: log_push("需要火把(工坊:1木1布)"); break;
                default: break;
            }
            s.dirty = true;
            break;
        }
        case PG_TRADE: {
            if (idx == 8) { page_goto(PG_HOME); break; }
            static const char *goods[DR_TRADE_KIND_COUNT] = {
                "鳞", "牙", "铁", "煤", "钢", "子弹", "药", "罗盘",
            };
            if (idx == DR_TRADE_COMPASS &&
                (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS))) {
                log_push("已经有罗盘了");
                break;
            }
            if (dr_rules_trade_buy(&s.game, (uint8_t)idx)) {
                char line[32];
                snprintf(line, sizeof(line), "买到 %s", goods[idx]);
                log_push(line);
                s.save_pending = true;
            } else {
                log_push("货款不够");
            }
            break;
        }
        case PG_CRAFT: {
            if (idx >= s.craft_row_count) { page_goto(PG_HOME); break; }
            uint8_t c = s.craft_row_map[idx];
            if (dr_rules_craft(&s.game, c)) {
                char line[40];
                snprintf(line, sizeof(line), "造成了 %s", craft_name(c));
                log_push(line);
                s.save_pending = true;
            } else {
                log_push("材料不够或需要工坊");
            }
            break;
        }
        case PG_SETTINGS:
            if (idx == 3) page_goto(PG_HOME);
            else if (idx == 0) log_push("上/下选择 · 确定执行 · 长按返回");
            else if (idx == 1) {                       // 重开本局:需确认
                s.confirm_from = 0;
                page_goto(PG_CONFIRM);
            }
            else log_push("《小黑屋》A Dark Room 重制");
            break;
        default:
            break;
    }
}

static void handle_ok(void) {
    switch (s.page) {
        case PG_TITLE: page_goto(PG_HOME); break;
        case PG_HOME:  home_action(s.focus); break;
        case PG_BUILD:
        case PG_VILLAGE:
        case PG_MAP:
        case PG_RUIN:
        case PG_COMBAT:
        case PG_TRADE:
        case PG_CRAFT:
        case PG_SETTINGS: list_action(s.focus); break;
        case PG_EVENT: {
            uint16_t count = 0;
            const dr_event_t *ev = dr_events_table(&count);
            int16_t chain = dr_event_choose(ev, count, &s.game, &s.ev_sess,
                                            dr_port_now_ts(), s.ev_idx, s.focus);
            s.save_pending = true;
            log_push(dr_text(ev[s.ev_idx].choices[s.focus].text_id));
            if (chain >= 0) {
                s.ev_idx = (uint16_t)chain;
                s.focus = 0;
            } else {
                page_goto(PG_HOME);
            }
            break;
        }
        case PG_CONFIRM:
            if (s.focus == 0) {
                if (s.confirm_from == 0) {
                    uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
                    dr_game_init(&s.game, seed, dr_port_now_ts());
                    s_log_cnt = 0;   // 新的一局:日志清零
                    log_push("新的一局开始了");
                }
                s.save_pending = true;
            }
            page_goto(s.confirm_from == 0 ? PG_SETTINGS : PG_TRADE);
            break;
    }
    s.dirty = true;
}

// ===================================================================
// 事件触发(必须走 page_goto)
// ===================================================================
static void check_event(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    dr_rng_t rng;
    dr_rng_seed(&rng, s.game.rng_seed_state ^ 0xC0FFEEu);
    uint16_t idx;
    if (dr_event_pick(ev, count, &s.game, &s.ev_sess, dr_port_now_ts(),
                      DR_SCENE_HOME, &rng, &idx) == DR_EVENT_FIRED) {
        s.game.rng_seed_state = rng.s;
        s.ev_idx = idx;
        page_goto(PG_EVENT);
    }
}

// ===================================================================
// 周期心跳
// ===================================================================
static void tick(lv_timer_t *t) {
    (void)t;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    static uint32_t last_batt_ms = 0;
    if (last_batt_ms == 0 || now_ms - last_batt_ms >= 30000u) {
        last_batt_ms = now_ms;
        int soc = bsp_battery_soc();
        // 标题页顶栏留白,电量只在游戏页刷新
        if (soc >= 0 && s.page != PG_TITLE) lv_label_set_text_fmt(s_batt, "%d%%", soc);
    }

    // 规则心跳:火焰/温度/建造者剧情/收入/流浪者;剧情瞬间写日志
    dr_rules_event_t ev;
    uint16_t arg;
    if (dr_rules_tick(&s.rules_rt, &s.game, now_ms, &ev, &arg)) {
        s.dirty = true;
        switch (ev) {
            case DR_RT_EV_BUILDER_IN:
                log_push("一个陌生人踉跄着走进来,倒下了");
                s.save_pending = true;
                break;
            case DR_RT_EV_FOREST:
                log_push("柴火快烧完了");
                log_push("森林敞开了大门");
                s.save_pending = true;
                break;
            case DR_RT_EV_BUILDER_SHIVER:
                log_push("她打着寒战,喃喃自语");
                break;
            case DR_RT_EV_BUILDER_SLEEP:
                log_push("她不再发抖,呼吸平稳下来");
                break;
            case DR_RT_EV_BUILDER_HELP:
                log_push("她站在火边:可以帮忙了");
                s.save_pending = true;
                break;
            case DR_RT_EV_BUILDER_STOKE:
                log_push("建造者往火里添了柴");
                s.save_pending = true;
                break;
            case DR_RT_EV_FIRE_OUT:
                log_push("火熄了,屋里冷下来");
                s.save_pending = true;
                break;
            case DR_RT_EV_FIRE_DOWN:
                log_push("火弱了下去");
                break;
            case DR_RT_EV_WANDERER:
                // 原版五档文案:1 陌生人 / <5 一家人 / <10 一小群 / <30 车队 / 其余 大批流民
                log_push(arg == 1 ? "一位流浪者在夜里住了下来"
                      : arg < 5 ? "一家人在荒野里找到了这里"
                      : arg < 10 ? "一小群人流浪到此"
                      : arg < 30 ? "一支车队安顿了下来"
                                 : "大批流民涌到了火光旁");
                s.save_pending = true;
                break;
            default:
                break;
        }
    }
    if (s.page == PG_HOME) {
        check_event();
        // 添柴冷却条随 250ms 心跳逐拍收短;冷却结束瞬间也要重绘一次清掉底色
        int32_t s_rem = (int32_t)(s.rules_rt.stoke_ready_ms - now_ms);
        if (s_rem > 0 || s.prev_stoke_active) s.dirty = true;
        s.prev_stoke_active = (s_rem > 0);
    }
    if (s.page == PG_VILLAGE) {
        // 采集/陷阱倒计时按秒变化才重绘(文字分辨率 1s,跳变即刷)
        int32_t g_rem = (int32_t)(s.rules_rt.gather_ready_ms - now_ms);
        int32_t t_rem = (int32_t)(s.rules_rt.trap_next_ms - now_ms);
        bool t_has = s.game.building_lv[DR_BLD_TRAP] > 0;
        bool g_act = (g_rem > 0), t_act = t_has && (t_rem > 0);
        if (g_act != s.prev_gather_ready || t_act != s.prev_trap_ready ||
            g_act || t_act)
            s.dirty = true;
        s.prev_gather_ready = g_act;
        s.prev_trap_ready = t_act;
    }
    // 周期存档:纯挂机时收入产出也落盘(与按键/渲染同在 LVGL
    // 线程,天然与状态变更互斥;NVS 写入约毫秒级,每分钟一次可接受)
    if (now_ms - s.autosave_ms >= 60000u) {
        s.autosave_ms = now_ms;
        save_all();
    }

    render();
}

// ===================================================================
// 按键
// ===================================================================
// tab 启用表:小屋/≡ 常开;村庄 = 森林剧情解锁;荒野 M4 未实装前恒锁
static bool forest_open(void) {
    return (s.game.flags & ((uint64_t)1u << DR_FLAG_FOREST)) != 0;
}
static bool nav_tab_enabled(int i) {
    switch (i) {
        case 1: return forest_open();
        case 2: return s.game.in_wilderness ||     // 远征中必须能看地图
                       s.game.res[DR_RES_FOOD] > 0; // 有干肉即可出发(原版口径)
        default: return true;
    }
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_LONG) return;
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN && btn != BSP_BTN_OK) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s.boot_ms < 1500u) return;  // 开机防抖

    if (ev == BSP_BTN_PRESS) {
        // 渲染静默期:渲染电流尖峰制造的幽灵按键紧贴渲染出现
        if (now_ms - s.last_render_ms < 60u) return;
        // 出发整备调配:上/下=±1,确定=完成
        if (s.page == PG_MAP && s.outfit_adj >= 0) {
            static const uint8_t outfit_res[3] = {
                DR_RES_FOOD, DR_RES_MEDICINE, DR_RES_BULLETS
            };
            if (btn == BSP_BTN_OK) {
                s.outfit_adj = -1;
                s.dirty = true;
                return;
            }
            int16_t d = (btn == BSP_BTN_UP) ? 1 :
                        (btn == BSP_BTN_DOWN) ? -1 : 0;
            if (d != 0) {
                if (!dr_world_outfit_add(&s_world, &s.game,
                                         outfit_res[s.outfit_adj], d))
                    log_push(d > 0 ? "背袋满了或库存不足" : "已经没有携带了");
                s.save_pending = true;
                s.dirty = true;
            }
            return;
        }
        // 村庄人数调节:上/下=±1,确定=完成退出
        if (s.page == PG_VILLAGE && s.village_adj >= 0) {
            if (btn == BSP_BTN_OK) {
                s.village_adj = -1;
                s.dirty = true;
                return;
            }
            int16_t d = (btn == BSP_BTN_UP) ? 1 :
                        (btn == BSP_BTN_DOWN) ? -1 : 0;
            if (d != 0) {
                if (!dr_rules_job_assign(&s.game, (uint8_t)s.village_adj, d))
                    log_push(d > 0 ? "没有闲着的人了" : "这个岗位已经没人了");
                s.save_pending = true;
                s.dirty = true;
            }
            return;
        }
        // 主页:导航栏(小屋/村庄/荒野/≡)是焦点首区。动作区顶端向上进入导航;
        // 导航内 UP 在启用的 tab 间循环(未启用跳过)、DOWN 回到动作区首行;
        // OK 切页
        if (s.page == PG_HOME && s.nav_focus >= 0) {
            if (btn == BSP_BTN_UP) {
                do {
                    s.nav_focus = (s.nav_focus + 3) % 4;
                } while (!nav_tab_enabled(s.nav_focus));
                s.dirty = true;
            } else if (btn == BSP_BTN_DOWN) {
                s.nav_focus = -1;
                s.focus = 0;
                s.dirty = true;
            } else if (btn == BSP_BTN_OK) {
                int t = s.nav_focus;
                bool go = true;
                switch (t) {
                    case 0: page_goto(PG_HOME); break;   // 已在小屋:仅退回动作区
                    case 1: if (forest_open()) page_goto(PG_VILLAGE);
                            else { log_push("森林尚未开启"); go = false; }
                            break;
                    case 2: log_push("荒野尚未解锁"); go = false; break;   // M4 解锁
                    case 3: page_goto(PG_SETTINGS); break;
                }
                if (go) { s.nav_focus = -1; s.focus = 0; }
                s.dirty = true;
            }
            return;
        }
        if (s.page == PG_HOME && btn == BSP_BTN_UP && s.focus == 0) {
            s.nav_focus = 0;   // 从动作区顶端向上进入导航栏
            s.dirty = true;
            return;
        }
        // 村庄页职业区滚动:窗口 5 行,到边缘且还有职业时先滚窗再移焦
        if (s.page == PG_VILLAGE && s.village_adj < 0 &&
            !s.game.in_wilderness) {
            if (btn == BSP_BTN_UP && s.focus == 3 && s.job_scroll > 0) {
                s.job_scroll--;
                s.dirty = true;
                return;
            }
            if (btn == BSP_BTN_DOWN && s.focus == 7 &&
                s.job_scroll + 5 < DR_JOB_KIND_COUNT) {
                s.job_scroll++;
                s.dirty = true;
                return;
            }
        }
        if (btn == BSP_BTN_UP) {
            focus_move(-1);
            s.dirty = true;
        } else if (btn == BSP_BTN_DOWN) {
            focus_move(1);
            s.dirty = true;
        } else {
            handle_ok();
        }
    } else if (ev == BSP_BTN_LONG) {
        // 村庄:长按确定=完成(不返回主页——PRESS 先进调节,长按返回必误触);
        // 调节中长按上/下 = ±5
        if (s.page == PG_VILLAGE) {
            if (btn == BSP_BTN_OK) {
                if (s.village_adj >= 0) {
                    s.village_adj = -1;
                    s.dirty = true;
                }
            } else if (s.village_adj >= 0) {
                int16_t d = (btn == BSP_BTN_UP) ? 5 : -5;
                if (!dr_rules_job_assign(&s.game, (uint8_t)s.village_adj, d))
                    log_push(d > 0 ? "没有闲着的人了" : "这个岗位已经没人了");
                s.save_pending = true;
                s.dirty = true;
            }
            return;
        }
        if (btn != BSP_BTN_OK) return;
        // 危险操作的长按快捷路径:同样走确认弹窗(与按下确定一致)
        if (s.page == PG_SETTINGS && s.focus == 1) {
            s.confirm_from = 0;
            page_goto(PG_CONFIRM);
            return;
        }
        // 长按返回;事件/战斗/确认页禁用(必须显式选择);村庄页不响应长按返回
        switch (s.page) {
            case PG_BUILD:
            case PG_TRADE:
            case PG_SETTINGS:
            case PG_CRAFT:
            case PG_MAP:    page_goto(PG_HOME); break;
            case PG_RUIN:   page_goto(PG_MAP);  break;
            default: break;
        }
    }
}

static void key_task(void *arg) {
    (void)arg;
    key_event_t ke;
    while (!s_key_quit) {
        if (xQueueReceive(s_key_queue, &ke, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (bsp_lvgl_lock(250)) {
                handle_key(ke.btn, ke.ev);
                render();
                bsp_lvgl_unlock();
            }
        }
        if (s.save_pending) {
            s.save_pending = false;
            save_all();
        }
    }
    s_key_task = NULL;
    vTaskDelete(NULL);
}

// ===================================================================
// 对外接口
// ===================================================================
void darkroom_app_enter(void) {
    memset(&s, 0, sizeof(s));
    s.nav_focus = -1;   // -1 = 焦点在动作区
    s.village_adj = -1; // -1 = 村庄页未进入人数调节
    s.outfit_adj = -1;  // -1 = 未在整备调配
    s.boot_ms = (uint32_t)(esp_timer_get_time() / 1000);

    dr_port_storage_init();
    bool loaded = false;
    dr_port_load(&s.game, &loaded);
    if (!loaded) {
        s_log_cnt = 0;   // 新档:日志也从零开始(s_logs 在 s 之外,须显式清)
        uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
        dr_game_init(&s.game, seed, dr_port_now_ts());
        save_all();
        ESP_LOGI(TAG, "新档已建(seed=%u)", seed);
    } else {
        // 日志随档恢复(独立键;缺失/损坏 = 空日志,不影响游戏)
        uint8_t lbuf[sizeof(dr_log_hdr_t) + 1u + sizeof(s_logs)];
        size_t llen = sizeof(lbuf);
        if (dr_port_log_load(lbuf, sizeof(lbuf), &llen) == 0 && llen > 0)
            log_restore(lbuf, llen);
    }

    dr_event_session_init(&s.ev_sess);
    dr_rules_rt_init(&s.rules_rt, &s.game, (uint32_t)(esp_timer_get_time() / 1000));

    // 离线结算:火焰熄灭 + 收入/陷阱补算(读档才有间隔)
    if (loaded) {
        dr_offline_yield_t oy;
        dr_rules_offline_settle(&s.rules_rt, &s.game, dr_port_now_ts(),
                                (uint32_t)(esp_timer_get_time() / 1000), &oy);
        if (oy.ticks) {
            // 锚点已被结算推进,立刻落盘——否则断电会让同一离线窗口重复结算
            s.save_pending = true;
            if (oy.wood || oy.fur || oy.meat || oy.leather || oy.food) {
                char line[64];
                snprintf(line, sizeof(line),
                         "离线:木+%lu 毛+%lu 肉+%lu 革+%lu 干+%lu",
                         (unsigned long)oy.wood, (unsigned long)oy.fur,
                         (unsigned long)oy.meat, (unsigned long)oy.leather,
                         (unsigned long)oy.food);
                log_push(line);
            }
            if (oy.fire_out) log_push("回来时火已经熄了");
        }
    }

    // 世界(种子派生,重启同图)与远征中断兜底:断电按死亡处理(物资已扣)
    dr_world_gen(&s_world, s.game.map_seed);
    if (loaded && s.game.in_wilderness) {
        dr_world_fail_trip(&s_world, &s.game);
        log_push("远征中断:你在荒野中失去了意识");
        s.save_pending = true;
    }

    build_ui();
    s_key_quit = false;
    s_key_queue = xQueueCreate(8, sizeof(key_event_t));
    xTaskCreate(key_task, "dr_key", 8192, NULL, 5, &s_key_task);
    s_timer = lv_timer_create(tick, 250, NULL);   // 250ms:冷却条平滑收短

    page_goto(PG_TITLE);
    render();
}

void darkroom_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 回调运行于 button 组件的 esp_timer 任务:只做非阻塞入队,不打日志、不读外设
    if (!s_key_queue) return;
    key_event_t ke = { .btn = btn, .ev = ev };
    xQueueSend(s_key_queue, &ke, 0);
}

void darkroom_app_stop(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    // 先让按键任务退出再删队列,避免任务阻塞在已删除的队列上
    if (s_key_task) {
        s_key_quit = true;
        for (int i = 0; i < 10 && s_key_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_key_queue) { vQueueDelete(s_key_queue); s_key_queue = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    save_all();
}

// 模拟器诊断:打印对象树关键状态(仅主机构建使用)
void dr_debug_dump(void) {
    ESP_LOGI(TAG, "dump page=%d focus=%d wood=%lu pop=%u fire=%u",
             (int)s.page, s.focus,
             (unsigned long)s.game.res[DR_RES_WOOD],
             s.game.population, s.game.fire_lv);
}

// 模拟器诊断:轮播下一页(SIM_PAGE_SWEEP 用)
void dr_sweep_next(void) {
    static const page_t order[] = {
        PG_TITLE, PG_HOME, PG_BUILD, PG_VILLAGE, PG_MAP, PG_RUIN,
        PG_COMBAT, PG_TRADE, PG_SETTINGS, PG_EVENT, PG_CONFIRM,
    };
    static int i = 0;
    page_goto(order[i % (sizeof(order) / sizeof(order[0]))]);
    if (s.page == PG_EVENT) s.ev_idx = 0;
    i++;
}
