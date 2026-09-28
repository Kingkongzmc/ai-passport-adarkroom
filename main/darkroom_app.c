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
    PG_TITLE = 0, PG_HOME, PG_BUILD, PG_VILLAGE, PG_MAP,
    PG_RUIN, PG_COMBAT, PG_TRADE, PG_SETTINGS,
    PG_EVENT, PG_CONFIRM,
} page_t;

// ---- 状态 ----
static struct {
    uint32_t boot_ms;          // 开机防抖基准
    uint32_t last_render_ms;   // 渲染静默期基准
    volatile bool save_pending;
    uint8_t confirm_from;      // 确认页来源(0=设置重开 1=贸易全卖)
    int8_t nav_focus;          // 主页导航焦点(-1=动作区,0..3=tab)
    int8_t village_adj;        // 村庄调节模式:在调的职业(-1=未调节)
    bool prev_gather_ready;    // 上一秒冷却是否已就绪(就绪瞬间触发重绘)
    bool prev_trap_ready;
    bool prev_starving;        // 上一秒是否断粮罢工(转变瞬间记日志)
    uint32_t autosave_ms;      // 周期存档基准(挂机产出也落盘,断电回滚≤1分钟)
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
#define LIST_ROWS 9
static listrow_t s_list[LIST_ROWS];
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

static const char *fire_char(void) {
    static const char *f[] = {"熄", "微", "跳", "旺", "炽"};
    uint8_t lv = s.game.fire_lv;
    if (lv > DR_FIRE_ROARING) lv = DR_FIRE_ROARING;
    return f[lv];
}

static const char *bld_short(int id) {
    static const char *n[] = {"板车", "陷阱", "小屋", "猎屋", "贸站", "革坊"};
    return (id >= 0 && id < 6) ? n[id] : "建筑";
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
    return s.game.building_lv[DR_BLD_TRAP] > 0 ? 4 : 3;
}

static int gather_pct(uint32_t now_ms) {
    int32_t rem = (int32_t)(s.rules_rt.gather_ready_ms - now_ms);
    if (rem <= 0) return 0;
    return (int)(rem * 100 / (DR_GATHER_COOLDOWN_S * 1000u));
}

static int trap_pct(uint32_t now_ms) {
    if (s.game.building_lv[DR_BLD_TRAP] == 0) return 0;
    int32_t rem = (int32_t)(s.rules_rt.trap_next_ms - now_ms);
    if (rem <= 0) return 0;
    return (int)(rem * 100 / (DR_TRAP_PERIOD_S * 1000u));
}

// 添柴/点火冷却(原版 Room._STOKE_COOLDOWN,点火添柴共用)
static int stoke_pct(uint32_t now_ms) {
    int32_t rem = (int32_t)(s.rules_rt.stoke_ready_ms - now_ms);
    if (rem <= 0) return 0;
    return (int)(rem * 100 / (DR_STOKE_COOLDOWN_S * 1000u));
}

static void render_home(uint32_t now_ms) {
    render_topbar("小黑屋");
    render_tabs(0, s.game.population > 0, false, s.nav_focus);
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
    int pct = gather_pct(now_ms);
    int gy = 184;
    bool act_focus = (s.nav_focus < 0);   // 光标在页签上时动作区整体去焦点
    set_act(&s_acts[idx], pct > 0 ? "收集木材…" : "收集木材",
            act_focus && s.focus == idx, pct);
    lv_obj_set_pos(s_acts[idx].row, 0, gy);
    gy += 24;
    idx++;
    if (s.game.building_lv[DR_BLD_TRAP] > 0) {
        int tp = trap_pct(now_ms);
        set_act(&s_acts[idx], tp > 0 ? "查看陷阱…" : "查看陷阱!",
                act_focus && s.focus == idx, tp);
        lv_obj_set_pos(s_acts[idx].row, 0, gy);
        gy += 24;
        idx++;
    }
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
    for (int i = 0; i < n; i++) lv_obj_clear_flag(s_acts[i].row, LV_OBJ_FLAG_HIDDEN);
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
        uint8_t lv = s.game.building_lv[i];
        bool can = dr_rules_can_build(&s.game, i);
        char t[24], v[28];
        dr_bld_cost_t c = dr_building_cost(i, lv);
        snprintf(t, sizeof(t), "%s Lv%u", bld_short(i), lv);
        if (can) {
            // 可建:完整造价(紧凑格式,避免数值列截断)
            if (c.fur && c.meat)
                snprintf(v, sizeof(v), "%u木%u毛%u肉", c.wood, c.fur, c.meat);
            else if (c.fur)
                snprintf(v, sizeof(v), "%u木%u毛", c.wood, c.fur);
            else
                snprintf(v, sizeof(v), "%u木", c.wood);
        } else {
            // 不可建:只列缺口(资源差值);资源够但前置不满足 → 未解锁
            uint32_t dw = (c.wood > s.game.res[DR_RES_WOOD])
                              ? c.wood - s.game.res[DR_RES_WOOD] : 0;
            uint32_t df = (c.fur > s.game.res[DR_RES_FUR])
                              ? c.fur - s.game.res[DR_RES_FUR] : 0;
            uint32_t dm = (c.meat > s.game.res[DR_RES_MEAT])
                              ? c.meat - s.game.res[DR_RES_MEAT] : 0;
            if (dw || df || dm) {
                int n = snprintf(v, sizeof(v), "缺");
                if (dw) n += snprintf(v + n, sizeof(v) - n, "%lu木",
                                      (unsigned long)dw);
                if (df) n += snprintf(v + n, sizeof(v) - n, "%lu毛",
                                      (unsigned long)df);
                if (dm) snprintf(v + n, sizeof(v) - n, "%lu肉",
                                 (unsigned long)dm);
            } else {
                snprintf(v, sizeof(v), "未解锁");
            }
        }
        set_row(i, 52 + i * 23, t, v, s.focus == i, !can);
    }
    set_row(DR_BLD_KIND_COUNT, 52 + DR_BLD_KIND_COUNT * 23, "返回", "",
            s.focus == DR_BLD_KIND_COUNT, false);
    // 底部说明:光标所选建筑的作用
    static const char *bld_desc[] = {
        "解锁陷阱制作",             // 板车
        "定期收获毛皮/肉",          // 陷阱
        "人口上限+2,流浪者入住",    // 小屋
        "解锁猎人职业",             // 猎人小屋
        "解锁贸易:毛/肉换木",       // 贸易站
        "解锁制革匠:毛皮变皮革",    // 制革坊
    };
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 230);
    lv_label_set_text(s_hint,
        s.focus < DR_BLD_KIND_COUNT ? bld_desc[s.focus] : "");
}

static void render_village(void) {
    render_topbar("村庄");
    render_tabs(1, true, false, -1);
    static const char *jobs[5] = {"伐木工", "猎人", "制革匠", "采集者", "铁匠"};
    uint16_t idle = dr_rules_job_idle(&s.game);
    char v[32];
    bool adj = (s.village_adj >= 0);
    // 行0..2 职业可调(确定进入人数调节);行3 采集者 = 未分配人口(自动拾柴觅食)
    snprintf(v, sizeof(v), "%u人 +2木", s.game.job[DR_JOB_LUMBER]);
    set_row(0, 72, jobs[0], v, s.focus == 0, false);
    bool hunter_ok = dr_rules_job_unlocked(&s.game, DR_JOB_HUNTER);
    if (hunter_ok) snprintf(v, sizeof(v), "%u人 +1毛2肉", s.game.job[DR_JOB_HUNTER]);
    else           snprintf(v, sizeof(v), "需猎人小屋");
    set_row(1, 96, jobs[1], v, s.focus == 1, !hunter_ok);
    bool tanner_ok = dr_rules_job_unlocked(&s.game, DR_JOB_TANNER);
    if (tanner_ok) snprintf(v, sizeof(v), "%u人 2毛换1革", s.game.job[DR_JOB_TANNER]);
    else           snprintf(v, sizeof(v), "需制革坊");
    set_row(2, 120, jobs[2], v, s.focus == 2, !tanner_ok);
    snprintf(v, sizeof(v), "%u人 +1木1食", idle);
    set_row(3, 144, jobs[3], v, s.focus == 3, false);
    set_row(4, 168, jobs[4], "需铁 M4", s.focus == 4, true);
    set_row(5, 192, adj ? "完成调节" : "返回", "", s.focus == 5, false);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 230);
    if (adj) {
        lv_label_set_text(s_hint, "上加 下减(长按=5) 确定=完成");
    } else if (dr_rules_starving(&s.game)) {
        lv_label_set_text(s_hint, "断粮罢工:村民只拾荒求生");
    } else {
        // 收入总览(每 10s 经济 tick):产出 + 口粮消耗
        uint32_t hunter = s.game.job[DR_JOB_HUNTER];
        uint32_t tanner = s.game.job[DR_JOB_TANNER];
        char hbuf[96];
        int n = snprintf(hbuf, sizeof(hbuf), "共%u人/10s 木+%lu",
                         s.game.population,
                         (unsigned long)(idle + (uint32_t)s.game.job[DR_JOB_LUMBER] * 2u));
        if (idle) n += snprintf(hbuf + n, sizeof(hbuf) - n, " 食+%lu",
                                (unsigned long)idle);
        if (hunter) n += snprintf(hbuf + n, sizeof(hbuf) - n, " 毛+%lu 肉+%lu",
                                  (unsigned long)hunter,
                                  (unsigned long)(hunter * DR_HUNTER_MEAT));
        if (tanner) n += snprintf(hbuf + n, sizeof(hbuf) - n, " 革+%lu",
                                  (unsigned long)tanner);
        snprintf(hbuf + n, sizeof(hbuf) - n, " 粮耗-%u", s.game.population);
        lv_label_set_text(s_hint, hbuf);
    }
}

static void render_map(void) {
    render_topbar("荒野");
    // 不显示导航页签(mockup ⑥):资源行占 y=24,与页签同带会文字叠文字
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "水 %u", s.game.water);
    lv_label_set_text_fmt(s_cells[1], "食 %u", s.game.food);
    lv_label_set_text(s_cells[2], "HP 8");
    lv_label_set_text(s_cells[3], "步 12");
    for (int i = 0; i < MAP_CELLS; i++) {
        lv_obj_clear_flag(s_map[i], LV_OBJ_FLAG_HIDDEN);
        int cx = i % 9, cy = i / 9;
        lv_color_t c = lv_color_hex(COL_BG);
        int dx = cx - 4, dy = cy - 4;
        if (cx == 4 && cy == 4) c = lv_color_hex(COL_ME);
        else if (cx == 3 && cy == 5) c = lv_color_hex(COL_GOLD);
        else if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) c = lv_color_hex(0x55524D);
        else if (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3) c = lv_color_hex(COL_CD);
        lv_obj_set_style_bg_color(s_map[i], c, 0);
    }
    lv_obj_clear_flag(s_legend[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_legend[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_legend[0], 0, 146);
    lv_obj_set_pos(s_legend[1], 0, 161);
    lv_label_set_text(s_legend[0], "■你 ■家 ■照亮");
    lv_label_set_text(s_legend[1], "■走过 ■未知");
    set_row(0, 188, "东 干涸河床 2格", "", s.focus == 0, false);
    set_row(1, 212, "北 未知", "", s.focus == 1, false);
    set_row(2, 236, "西 小屋 3格", "回", s.focus == 2, false);
}

static void render_ruin(void) {
    render_topbar("废村");
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_cells[0], "水 4");
    lv_label_set_text(s_cells[1], "食 3");
    lv_label_set_text(s_cells[2], "HP 8");
    lv_label_set_text(s_cells[3], "包 5/8");
    for (int i = 0; i < ROOM_CELLS; i++) {
        lv_obj_clear_flag(s_room[i], LV_OBJ_FLAG_HIDDEN);
        lv_color_t c = lv_color_hex(COL_BG);
        if (i == 2) c = lv_color_hex(COL_ME);
        else if (i == 4) c = lv_color_hex(COL_ENEMY);
        else if (i < 6) c = lv_color_hex(COL_CD);
        lv_obj_set_style_bg_color(s_room[i], c, 0);
    }
    lv_obj_clear_flag(s_legend[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_legend[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_legend[0], 0, 132);
    lv_obj_set_pos(s_legend[1], 0, 147);
    lv_label_set_text(s_legend[0], "■你 ■敌人");
    lv_label_set_text(s_legend[1], "■已搜 ■未搜");
    set_row(0, 158, "东 房间(未搜)", "", s.focus == 0, false);
    set_row(1, 182, "南 房间(空)", "", s.focus == 1, false);
    set_row(2, 206, "西北 房间(敌人)", "遭遇", s.focus == 2, false);
    set_row(3, 230, "离开地点", "", s.focus == 3, false);
}

static void render_combat(void) {
    render_topbar("战斗");
    // 敌我名行(198 通栏)+ 血条,间距按 mockup ⑦:名30/条46/名62/条78
    for (int i = 2; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_base(0, 0, 30);
    cell_base(1, 0, 62);
    lv_obj_set_size(s_cells[0], 198, 15);
    lv_obj_set_size(s_cells[1], 198, 15);
    lv_obj_clear_flag(s_cells[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cells[1], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_cells[0], "巨鼠 66%");
    lv_label_set_text(s_cells[1], "你(铁剑) 83%");
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_flag(s_hp[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hp[i], 0, 46 + i * 32);
        lv_obj_set_size(s_hpfill[i], i == 0 ? 131 : 164, 8);
    }
    // 战斗日志:占位 M4,两行灰字
    lv_obj_set_pos(s_loglines[0], 0, 100);
    lv_obj_set_pos(s_loglines[1], 0, 100 + lv_font_get_line_height(&dr_font_12));
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_loglines[i], lv_color_hex(COL_DIM), 0);
    }
    lv_label_set_text(s_loglines[0], "你挥剑命中,造成 6 伤害");
    lv_label_set_text(s_loglines[1], "巨鼠咬中你,造成 2 伤害");
    set_act(&s_acts[0], "攻击(铁剑 4-7)", s.focus == 0, 0);
    set_act(&s_acts[1], "换武器(钢剑 8-12)", s.focus == 1, 0);
    set_act(&s_acts[2], "补给(食+3)", s.focus == 2, 0);
    set_act(&s_acts[3], "逃跑(70%)", s.focus == 3, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_pos(s_acts[i].row, 0, 168 + i * 24);
        lv_obj_clear_flag(s_acts[i].row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_trade(void) {
    render_topbar("贸易");
    // 资源摘要单行 @24(mockup ⑧);列表行从 48 起,8 格带会与之重叠。
    // 第 4 格显示皮革:皮甲交易在本页最关心革存量。
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "木 %lu", (unsigned long)s.game.res[DR_RES_WOOD]);
    lv_label_set_text_fmt(s_cells[1], "毛 %lu", (unsigned long)s.game.res[DR_RES_FUR]);
    lv_label_set_text_fmt(s_cells[2], "肉 %lu", (unsigned long)s.game.res[DR_RES_MEAT]);
    lv_label_set_text_fmt(s_cells[3], "革 %lu", (unsigned long)s.game.res[DR_RES_LEATHER]);
    // 贸易站是商路前提:未建时交易行全部锁定
    bool post = s.game.building_lv[DR_BLD_TRADE_POST] > 0;
    bool owned = s.game.armor_lv > 0;
    char v[24];
    if (owned) snprintf(v, sizeof(v), "已穿着");
    else       snprintf(v, sizeof(v), "%u木%u革",
                        (unsigned)DR_TRADE_ARMOR_WOOD, (unsigned)DR_TRADE_ARMOR_LEATHER);
    set_row(0, 48, "卖 毛皮×10", "得 50木", s.focus == 0, !post);
    set_row(1, 72, "卖 肉×10", "得 30木", s.focus == 1, !post);
    set_row(2, 96, "买 诱饵×5", "花 15木", s.focus == 2, !post);
    set_row(3, 120, "买 皮甲", v, s.focus == 3, !post || owned);
    set_row(4, 144, "全部卖出", post ? "需确认" : "需贸易站", s.focus == 4, !post);
    set_row(5, 168, "返回", "", s.focus == 5, false);
}

static void render_settings(void) {
    render_topbar("设置");
    render_tabs(3, s.game.population > 0, false, -1);
    set_row(0, 52, "操作说明", "键位:三键", s.focus == 0, false);
    set_row(1, 76, "陷阱诱饵", s.game.trap_bait_on ? "开" : "关", s.focus == 1, false);
    set_row(2, 100, "重开本局", "需确认", s.focus == 2, false);
    set_row(3, 124, "关于", "v0.3", s.focus == 3, false);
    set_row(4, 148, "返回", "", s.focus == 4, false);
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
    char body[96];
    if (s.confirm_from == 0)
        snprintf(body, sizeof(body), "确定重开本局?当前进度将丢失。");
    else
        snprintf(body, sizeof(body), "毛皮×%lu 肉×%lu 全部卖出,得 %lu 木。",
                 (unsigned long)s.game.res[DR_RES_FUR],
                 (unsigned long)s.game.res[DR_RES_MEAT],
                 (unsigned long)dr_rules_trade_sell_value(&s.game));
    set_dbody(body);
    for (int i = 0; i < 2; i++) {
        actrow_t *a = &s_dacts[i];
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(a->row, 0, 130 + i * 24);
        set_act(a, i == 0 ? "确认执行" : "取消", s.focus == i, 0);
    }
    for (int i = 2; i < 4; i++)
        lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
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
        case PG_VILLAGE: return 6;
        case PG_MAP: return 3;
        case PG_RUIN: return 4;
        case PG_COMBAT: return 4;
        case PG_TRADE: return 6;
        case PG_SETTINGS: return 5;
        case PG_EVENT: {
            uint16_t c = 0;
            const dr_event_t *ev = dr_events_table(&c);
            return ev[s.ev_idx].choice_count;
        }
        case PG_CONFIRM: return 2;
        default: return 1;
    }
}

// 行可用性:禁用行(锁定/资源不足)被光标跳过,只保留视觉灰显说明原因。
static bool row_enabled(int idx) {
    switch (s.page) {
        case PG_BUILD:
            return idx == DR_BLD_KIND_COUNT ||          // 返回
                   dr_rules_can_build(&s.game, idx);
        case PG_VILLAGE:
            if (idx == 5 || idx == 0 || idx == 3) return true;
            if (idx == 1) return dr_rules_job_unlocked(&s.game, DR_JOB_HUNTER);
            if (idx == 2) return dr_rules_job_unlocked(&s.game, DR_JOB_TANNER);
            return false;                               // 铁匠 M4
        case PG_TRADE: {
            if (idx == 5) return true;
            if (s.game.building_lv[DR_BLD_TRADE_POST] == 0) return false;
            switch (idx) {
                case 0: return s.game.res[DR_RES_FUR] >= 10;
                case 1: return s.game.res[DR_RES_MEAT] >= 10;
                case 2: return s.game.res[DR_RES_WOOD] >= 15;
                case 3: return s.game.armor_lv == 0 &&
                               s.game.res[DR_RES_WOOD] >= DR_TRADE_ARMOR_WOOD &&
                               s.game.res[DR_RES_LEATHER] >= DR_TRADE_ARMOR_LEATHER;
                default: return dr_rules_trade_sell_value(&s.game) > 0;
            }
        }
        default:
            return true;   // 主页动作/设置/地图/废村/弹窗选项均无禁用态
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
    if (!row_enabled(0)) focus_move(1);   // 首行禁用则落到首个可用行
    s.dirty = true;
}

static void home_action(int idx) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool has_trap = s.game.building_lv[DR_BLD_TRAP] > 0;
    int i = 0;
    if (idx == i++) {                      // 收集木材
        if ((int32_t)(s.rules_rt.gather_ready_ms - now_ms) > 0) {
            log_push("还需要等等才能再收集");
        } else {
            dr_rules_gather(&s.game, dr_port_now_ts());
            s.rules_rt.gather_ready_ms = now_ms + DR_GATHER_COOLDOWN_S * 1000u;
            log_push("收集到 10 木");
            s.save_pending = true;
        }
        return;
    }
    if (has_trap && idx == i++) {          // 查看陷阱
        dr_trap_yield_t y;
        if (!dr_rules_trap_check(&s.rules_rt, &s.game, now_ms, &y)) {
            log_push("刚查过陷阱,再等等");
        } else {
            char line[48];
            int n = snprintf(line, sizeof(line), "陷阱收获:");
            if (n > 0 && n < (int)sizeof(line)) {
                if (y.fur)
                    n += snprintf(line + n, sizeof(line) - n, " 毛皮×%u", y.fur);
                if (n > 0 && n < (int)sizeof(line) && y.meat)
                    n += snprintf(line + n, sizeof(line) - n, " 肉×%u", y.meat);
                if (n > 0 && n < (int)sizeof(line) && y.bait)
                    snprintf(line + n, sizeof(line) - n, "(耗%u饵)", y.bait);
            }
            log_push(line);
            s.save_pending = true;
        }
        return;
    }
    if (idx == i++) {                      // 点火/添柴(共用 10s 冷却,原版 _STOKE_COOLDOWN)
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
    page_goto(PG_BUILD);                   // 建造…
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
            } else log_push("条件不满足或材料不够");
            break;
        case PG_VILLAGE:
            if (idx == 5) page_goto(PG_HOME);
            else if (idx == 0 || idx == 1 || idx == 2) {
                if (!dr_rules_job_unlocked(&s.game, idx)) {
                    log_push(idx == 1 ? "需先建猎人小屋" : "需先建制革坊");
                } else {
                    s.village_adj = (int8_t)idx;   // 进入人数调节
                    s.dirty = true;
                }
            }
            else if (idx == 3) log_push("闲人自动拾柴觅食(+1木1食)");
            else log_push("铁匠要等铁矿,M4 开工");
            break;
        case PG_MAP:
            if (idx == 2) page_goto(PG_HOME);
            else if (idx == 0) page_goto(PG_RUIN);
            else log_push("未知的方向");
            break;
        case PG_RUIN:
            if (idx == 3) page_goto(PG_MAP);
            else log_push("房间系统 M4 实装");
            break;
        case PG_COMBAT:
            log_push("战斗系统 M4 实装");
            break;
        case PG_TRADE: {
            if (idx == 5) { page_goto(PG_HOME); break; }
            if (s.game.building_lv[DR_BLD_TRADE_POST] == 0) {
                log_push("需先建贸易站");
                break;
            }
            bool done = false;
            if (idx == 0) done = dr_rules_trade_fur10(&s.game);
            else if (idx == 1) done = dr_rules_trade_meat10(&s.game);
            else if (idx == 2) done = dr_rules_trade_bait5(&s.game);
            else if (idx == 3) {                       // 买皮甲:50木+10革
                if (s.game.armor_lv > 0) log_push("已经穿着皮甲了");
                else if (dr_rules_trade_armor(&s.game)) {
                    log_push("买下皮甲,穿在身上(战斗减免 M4 生效)");
                    s.save_pending = true;
                } else {
                    log_push(s.game.res[DR_RES_LEATHER] < DR_TRADE_ARMOR_LEATHER
                                 ? "皮革不够(制革匠产革)" : "木头不够");
                }
                break;
            }
            else if (idx == 4) {                       // 全部卖出:需确认
                if (dr_rules_trade_sell_value(&s.game) == 0)
                    log_push("没有可卖的毛皮和肉");
                else {
                    s.confirm_from = 1;
                    page_goto(PG_CONFIRM);
                }
                break;
            }
            if (done) {
                log_push(idx == 0 ? "卖出毛皮×10 得 50木"
                          : idx == 1 ? "卖出肉×10 得 30木"
                                     : "买到诱饵×5");
                s.save_pending = true;
            } else {
                log_push(idx == 2 ? "木头不够" : "存货不足 10 个");
            }
            break;
        }
        case PG_SETTINGS:
            if (idx == 4) page_goto(PG_HOME);
            else if (idx == 0) log_push("上/下选择 · 确定执行 · 长按返回");
            else if (idx == 1) {                       // 陷阱诱饵开关
                s.game.trap_bait_on = !s.game.trap_bait_on;
                log_push(s.game.trap_bait_on
                             ? "诱饵已备上:查看陷阱每陷阱多掷一件"
                             : "诱饵已收起");
                s.save_pending = true;
            }
            else if (idx == 2) {                       // 重开本局:需确认
                s.confirm_from = 0;
                page_goto(PG_CONFIRM);
            }
            else log_push("待实装");
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
                    log_push("新的一局开始了");
                } else {
                    uint32_t gain = dr_rules_trade_sell_all(&s.game);
                    char line[48];
                    snprintf(line, sizeof(line), "全部卖出 得 %lu 木",
                             (unsigned long)gain);
                    log_push(line);
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

    bool fire_changed = false;
    uint16_t pop_before = s.game.population;
    if (dr_rules_tick(&s.rules_rt, &s.game, now_ms, &fire_changed)) {
        s.dirty = true;
        if (fire_changed) {
            log_push(s.game.fire_lv == DR_FIRE_DEAD ? "火熄了,屋里冷下来" : "火弱了下去");
            s.save_pending = true;
        }
        if (s.game.population > pop_before) {
            log_push("一位流浪者到来,住进了小屋");
            s.save_pending = true;
        }
    }
    // 断粮罢工的转变瞬间记日志(持续态由村庄页提示行展示)
    bool starving = dr_rules_starving(&s.game);
    if (starving != s.prev_starving) {
        log_push(starving ? "粮食见底,村民停了活计"
                          : "吃了顿饱饭,村里重新开工");
        s.prev_starving = starving;
        s.dirty = true;
        s.save_pending = true;
    }
    if (s.page == PG_HOME) {
        check_event();
        int32_t g_rem = (int32_t)(s.rules_rt.gather_ready_ms - now_ms);
        int32_t t_rem = (int32_t)(s.rules_rt.trap_next_ms - now_ms);
        int32_t s_rem = (int32_t)(s.rules_rt.stoke_ready_ms - now_ms);
        bool t_has = s.game.building_lv[DR_BLD_TRAP] > 0;
        // 冷却条逐秒刷新;且"就绪瞬间"(进行中→就绪)必须重绘一次,
        // 否则进度条冻在最后一格,直到下次按键才消失
        if (g_rem > 0 || s_rem > 0 || (t_has && t_rem > 0) ||
            (s.prev_gather_ready != (g_rem <= 0)) ||
            (t_has && s.prev_trap_ready != (t_rem <= 0)))
            s.dirty = true;
        s.prev_gather_ready = (g_rem <= 0);
        s.prev_trap_ready = t_has && (t_rem <= 0);
    }
    // 周期存档:纯挂机时经济 tick 的产出也落盘(与按键/渲染同在 LVGL
    // 线程,天然与状态变更互斥;NVS 写入约毫秒级,每分钟一次可接受)
    if (now_ms - s.autosave_ms >= 60000u) {
        s.autosave_ms = now_ms;
        dr_port_save(&s.game);
    }

    render();
}

// ===================================================================
// 按键
// ===================================================================
// tab 启用表:小屋/≡ 常开;村庄需有人;荒野 M4 未实装前恒锁
static bool nav_tab_enabled(int i) {
    switch (i) {
        case 1: return s.game.population > 0;
        case 2: return false;
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
                    case 1: if (s.game.population > 0) page_goto(PG_VILLAGE);
                            else { log_push("村庄尚未有人定居"); go = false; }
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
        if (s.page == PG_SETTINGS && s.focus == 2) {
            s.confirm_from = 0;
            page_goto(PG_CONFIRM);
            return;
        }
        if (s.page == PG_TRADE && s.focus == 4) {
            if (s.game.building_lv[DR_BLD_TRADE_POST] == 0)
                log_push("需先建贸易站");
            else if (dr_rules_trade_sell_value(&s.game) == 0)
                log_push("没有可卖的毛皮和肉");
            else {
                s.confirm_from = 1;
                page_goto(PG_CONFIRM);
            }
            return;
        }
        // 长按返回;事件/战斗/确认页禁用(必须显式选择);村庄页不响应长按返回
        switch (s.page) {
            case PG_BUILD:
            case PG_TRADE:
            case PG_SETTINGS:
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
            dr_port_save(&s.game);
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
    s.boot_ms = (uint32_t)(esp_timer_get_time() / 1000);

    dr_port_storage_init();
    bool loaded = false;
    dr_port_load(&s.game, &loaded);
    if (!loaded) {
        uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
        dr_game_init(&s.game, seed, dr_port_now_ts());
        dr_port_save(&s.game);
        ESP_LOGI(TAG, "新档已建(seed=%u)", seed);
    }

    dr_event_session_init(&s.ev_sess);
    dr_rules_rt_init(&s.rules_rt, &s.game, (uint32_t)(esp_timer_get_time() / 1000));

    // 离线结算:火焰熄灭 + 村庄产出/口粮 + 陷阱周期收获(读档才有间隔)
    if (loaded) {
        dr_offline_yield_t oy;
        dr_rules_offline_settle(&s.rules_rt, &s.game, dr_port_now_ts(),
                                (uint32_t)(esp_timer_get_time() / 1000), &oy);
        if (oy.ticks) {
            // 锚点已被结算推进,立刻落盘——否则断电会让同一离线窗口重复结算
            s.save_pending = true;
            if (oy.wood || oy.fur || oy.meat || oy.leather) {
                char line[64];
                snprintf(line, sizeof(line),
                         "离线:木+%lu 毛+%lu 肉+%lu 革+%lu",
                         (unsigned long)oy.wood, (unsigned long)oy.fur,
                         (unsigned long)oy.meat, (unsigned long)oy.leather);
                log_push(line);
            }
            if (oy.fire_out) log_push("回来时火已经熄了");
            if (oy.starving) log_push("离线时断了粮,村民罢工了");
        }
    }
    s.prev_starving = dr_rules_starving(&s.game);

    build_ui();
    s_key_quit = false;
    s_key_queue = xQueueCreate(8, sizeof(key_event_t));
    xTaskCreate(key_task, "dr_key", 8192, NULL, 5, &s_key_task);
    s_timer = lv_timer_create(tick, 1000, NULL);

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
    dr_port_save(&s.game);
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
