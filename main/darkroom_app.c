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
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "esp_heap_caps.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "dr_events.h"
#include "dr_port.h"
#include "dr_savecode.h"
#include "dr_text.h"
#include "dr_util.h"
#include "dr_world.h"
#include "esp_sleep.h"
#include "lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

LV_FONT_DECLARE(dr_font_12);
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
#define COL_ME     0x66B2FF
#define COL_ENEMY  0xE43B2F
// 美化 T1/T2:反色墨字 + 中性分隔线 + 页面 accent(自地形色板提亮,暗于金)
#define COL_INK    0x0D0D0D   // 焦点反色行上的文字色(=COL_BG)
#define COL_RULE   0x33302C   // 中性分隔线/地图格边(原散落字面量收编)
#define COL_AMBER  0xE08A3C   // HP 中量段/标题页火光带/主页 accent
#define ACCENT_HOME   0xE08A3C   // 主页:火光暖橙
#define ACCENT_FOREST 0x7FB55C   // 村庄/森林:森林 0x2E5E1E 提亮
#define ACCENT_WILD   0xC7A87E   // 荒野:荒地 0x967850 提亮
#define ACCENT_BUILD  0xC9986B   // 建造/老屋:0x965F37 提亮
#define ACCENT_TRADE  0x6FC9C9   // 贸易:哨站 0x9AE8E8 压暗半档
#define ACCENT_CRAFT  0xA9A9B5   // 制造/制造台:铁矿 0x707078 提亮
#define ACCENT_SHIP   0xD49BE8   // 星舰:0xC060D8 提亮(第四幕身份色)

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

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    uint32_t ms;   // 事件产生时刻(入队时间戳;幽灵窗按此判定,见 handle_key)
} key_event_t;

// ---- 页面 ----
typedef enum {
    PG_TITLE = 0, PG_HOME, PG_BUILD, PG_VILLAGE, PG_MAP,    PG_RUIN, PG_COMBAT, PG_TRADE, PG_SETTINGS,
    PG_EVENT, PG_CONFIRM, PG_CRAFT,
    PG_SHIP, PG_FAB, PG_LIFTOFF, PG_ENDING,   // M5 第四幕
    PG_EXPORT, PG_IMPORT,                     // M7 存档码(设置页进出)
} page_t;

// ---- 状态 ----
static struct {
    uint32_t boot_ms;          // 开机防抖基准
    uint32_t last_render_ms;   // 渲染静默期基准
    volatile bool save_pending;
    uint8_t confirm_from;      // 确认页来源(0=设置重开 1=贸易全卖 2=升空首飞警告)
    int8_t nav_focus;          // 主页导航焦点(-1=动作区,0..4=tab)
    int8_t village_adj;        // 村庄调节模式:在调的职业(-1=未调节)
    uint8_t job_scroll;        // 村庄职业区滚动偏移(窗口 5 行)
    uint8_t craft_scroll;      // 制造页滚动偏移(窗口 8 行,14 项装不下)
    uint8_t craft_total;       // 制造页当前可见制造项总数
    uint8_t craft_row_map[16]; // 制造页:行号 → 制造项
    uint8_t craft_row_count;   // 制造页:可见制造行数(不含返回)
    bool prev_gather_ready;    // 上一拍冷却秒数(变化即重绘:冷却条/倒计时逐拍走)
    bool prev_trap_ready;
    bool prev_stoke_active;    // 主页添柴冷却条活动态(结束瞬间也要重绘一次)
    uint8_t prev_stoke_sec;    // 冷却显示秒(按秒重绘;I34 幽灵窗吞键修复)
    uint8_t prev_gather_sec;
    uint8_t prev_trap_sec;
    uint32_t autosave_ms;      // 周期存档基准(挂机产出也落盘,断电回滚≤1分钟)
    uint32_t embark_cd_ms;     // 出发冷却到期时刻(死亡后 120s,原版口径)
    uint32_t liftoff_cd_ms;    // 升空冷却到期时刻(120s,原版 ship.js)
    uint8_t lift_seg;          // 升空叙事:当前段号(0=起飞)
    uint8_t lift_total;        // 升空叙事:总段数(8 固定 + 磨难段)
    int8_t fight_return;       // 战斗结束回页(默认 PG_MAP;执行者= PG_SHIP)
    int8_t last_dir;           // 荒野上次行进方向(0..3;-1=无;I35 焦点记忆)
    int8_t outfit_adj;         // 出发整备:正在调配的物资(-1=无)
    bool screen_off;           // 熄屏态(背光 0,任意键唤醒并吞键,DESIGN §8.1)
    uint32_t last_input_ms;    // 最近一次真实按键(自动熄屏计时基准)
    bool bl_dim;               // 电量≤20% 降背光态(唤醒/熄屏统一走 apply_backlight)
    bool batt_low_toast_done;  // ≤20% 提示只发一次
    volatile bool sleep_req;   // L2 请求(tick 置位;key_task 打盹循环消费)
    volatile bool deep_req;    // L3 请求(5min 到点 / 电量≤10%;key_task 执行)
    // 存档码(M7):导出全码缓存 + 页游标;导入逐字符状态机
    char code_buf[DR_CODE_MAX_CHARS + 1];
    int code_len;              // 导出码字符数
    int code_pages;            // 总页数(6 字/页)
    int code_page;             // 当前页 0 基
    char imp_buf[DR_CODE_MAX_CHARS];
    int imp_pos;               // 已确认字符数(光标位)
    int imp_total;             // 预期总字符(0=帧头未解出)
    int imp_draft;             // 光标位草稿在 base32 表中的下标
    bool imp_failed;           // 校验失败态(确定=清空重来,长按=退出)
    char toast[40];            // 就地结果提示(覆盖当前页提示行,到期还原)
    uint32_t toast_until_ms;
    uint32_t toast_col;        // 提示条颜色(T4:金=反馈,红=威胁)
    uint8_t dpanel_drop;       // 弹窗上滑入场剩余拍数(T7a:8/4/0 三档)
    bool log_focus;            // 日志窗聚焦态(T8:光标停在日志窗,未选中)
    bool log_browse;           // 日志窗选中态(T8:OK 选中后上下滚列表;再按 OK 取消)
    uint8_t log_off;           // 选中态浏览偏移(0=最新在顶,上限 cnt-8)
    dr_game_t game;
    dr_rules_rt_t rules_rt;
    dr_event_session_t ev_sess;
    dr_rng_t ev_rng;         // 事件选项的 GOTO_W 掷点流(会话内;不落盘)
    page_t page;
    int focus;
    uint16_t ev_idx;
    uint8_t ev_from;         // 事件弹出来源页(解决后回去;0=PG_TITLE 视同 HOME)
    bool dirty;
} s;

// ---- UI 对象 ----
static lv_obj_t *s_scr, *s_panel, *s_content;
static lv_obj_t *s_topbar, *s_batt;
static lv_obj_t *s_tabs[5], *s_tablbl[5];   // M5:第 4 页签=星舰(原版 An Old Starship)
static lv_obj_t *s_cells[8];
static lv_obj_t *s_loglines[LOG_VIS];
static lv_obj_t *s_hint;
static lv_obj_t *s_rule, *s_tick;   // 页头规则线+accent 短色条(T2,最底层)
static lv_obj_t *s_logpanel, *s_jobpanel;   // 日志/职业窗内嵌面板(T5,最底层)
static lv_obj_t *s_sep;             // 设置页危险项分隔线(T5)
static lv_obj_t *s_logtick;         // 主页最新日志锚点条(T7e,最底层)
static lv_obj_t *s_focusbar;        // 共享反色金条(T7d,最底层透显)
static bool s_veil_fading;          // 切页淡入进行中(T7a;clear_scene 让路)
static lv_obj_t *s_strack, *s_sthumb;   // 滚动指示条:右缘轨道+滑块(村庄/制造窗)
// 动作行:容器 + 冷却填充 + 箭头 + 文字(主页/战斗/弹窗选项共用骨架)
typedef struct {
    lv_obj_t *row, *fill, *arrow, *lbl;
} actrow_t;
// 金条归属与滑动状态(T7d):每页至多一行持焦(set_act/set_row 注册,最后写者
// 胜);页面存续期由 render() 尾统一驱动位置与 80ms y 补间,切页由 page_goto 收场
static actrow_t *s_bar_act;
static int s_bar_row = -1;
static bool s_bar_shown;
static int s_bar_last_y;
static bool s_bar_moving;
static void bar_y_cb(void *v, int32_t y);
static void bar_move_done_cb(lv_anim_t *a);
#define ACT_ROWS 4
static actrow_t s_acts[ACT_ROWS];
// 列表行:箭头 + 名称 + 值(建造/村庄/贸易/设置/地图目的地/房间)
typedef struct {
    lv_obj_t *row, *mark, *t, *v;
} listrow_t;
#define LIST_ROWS 12
static listrow_t s_list[LIST_ROWS];
static dr_world_t s_world;   // 世界与远征运行态(会话内;种子派生,不落盘)
// 弹窗层
static lv_obj_t *s_veil, *s_dpanel, *s_dtitle, *s_dbody;
static actrow_t s_dacts[5];   // 弹窗选项行(M6 切片二:游牧商人 5 选项)
// 地图/房间格阵 + 图例 + 血条
#define MAP_CELLS 81
static lv_obj_t *s_map[MAP_CELLS];
#define LEG_KINDS 18                   // 图例:全量地块色(地形/村庄/主角/矿/地点)
static lv_obj_t *s_legchip[LEG_KINDS], *s_legtxt[LEG_KINDS];
#define ROOM_CELLS 9
static lv_obj_t *s_room[ROOM_CELLS];
static lv_obj_t *s_hp[2], *s_hpfill[2];
static lv_obj_t *s_hptick[6];   // 血条 25/50/75% 刻度(美化 T4;子对象随条显隐)

// 存档码页(M7):导出 6 字符大间隔 + 页码行;导入 3 行回看(每行 6 字符)
#define CODE_COLS 6
static lv_obj_t *s_code_hdr;               // "第 n/N 页"
static lv_obj_t *s_code_char[CODE_COLS];   // 导出当前页字符(间隔 32px 拍照可辨)
static lv_obj_t *s_imp_lines[3];           // 导入回看行(最近 3 行 × 6 字符)

static lv_timer_t *s_timer;
static QueueHandle_t s_key_queue;
static TaskHandle_t s_key_task;
static volatile bool s_key_quit;

#define LOG_LINES 32       // 主页日志环形缓冲条数(浏览模式可回看,可视窗 8)
static char s_logs[LOG_LINES][48];
static int s_log_cnt;
// 语义主语色(美化 T4):并行会话内标记,NVS 不持久——重启恢复的旧日志
// 一律按系统色渲染,打包格式("DLO2")不动
#define LOG_SYS 0u
#define LOG_ME  1u
#define LOG_FOE 2u
static uint8_t s_logs_origin[LOG_LINES];

static void log_push_o(const char *text, uint8_t origin) {
    if (s_log_cnt == LOG_LINES) {
        memmove(s_logs[0], s_logs[1], sizeof(s_logs) - sizeof(s_logs[0]));
        memmove(s_logs_origin, s_logs_origin + 1, LOG_LINES - 1);
    } else
        s_log_cnt++;
    snprintf(s_logs[s_log_cnt - 1], sizeof(s_logs[0]), "%s", text);
    s_logs_origin[s_log_cnt - 1] = origin;
    s.dirty = true;
}

static void log_push(const char *text) { log_push_o(text, LOG_SYS); }

// 就地结果提示:把当前页底部提示行覆盖为彩色文字(约 2s,tick 到时还原)。
// 与 log_push 并用——即时反馈进提示行,历史留主页日志。
// 隐藏提示行的页面(主页有日志区/弹窗有正文)不显示,静默降级为仅日志。
// T4:常规反馈金,危险类(死亡/战败/断供/深睡预告)红——红=威胁,金=反馈
static void toast_c(const char *txt, uint32_t col) {
    snprintf(s.toast, sizeof(s.toast), "%s", txt);
    s.toast_col = col;
    s.toast_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 2000u;
    s.dirty = true;
}

static void toast(const char *txt) { toast_c(txt, COL_GOLD); }

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
    memset(s_logs_origin, LOG_SYS, sizeof(s_logs_origin));   // 旧档无主语色
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
                              "熏房", "钢厂", "械库", "工坊"};
    return (id >= 0 && id < 10) ? n[id] : "建筑";
}

// 村庄页签门:森林剧情解锁(原版 unlockForest)
static bool forest_open(void);
static bool ship_open(void);                // 星舰页签门(DR_FLAG_SHIP_OPEN)
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
    // 固定高度不得低于字体行高:LV_LABEL_LONG_DOT 下内容塞不进固定高度会
    // 触发"布局完成→重算文本"死循环,堆被逐轮 realloc 耗干后 lv_realloc
    // 挂死(真机 2026-10-05"出发远征"卡死根因,dr_key 持锁连带全键失灵)
    if (h < (int)font->line_height) h = font->line_height;
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

// 焦点样式 = 金色文字 + 行首固定宽度箭头列(独立 label,">"/空)。
// 前缀不在正文里:比例字体下 "> " 与两空格不等宽,会导致正文横移。

// 箭头列宽(全应用统一 f12,箭头列取 10),正文 x 统一 = 行 x + 箭头列宽。
#define ARROW_W12 10

// 手工折行:按显示宽度断行(ASCII 半格、其余整格),插入 '\n'。
// 弹窗正文 f12 在 176px 内每行 line_units 格;配合 LONG_DOT 使用,
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
    // 176px / 12px = 14 个整格字;单位为半角格(汉字占 2),故传 28
    wrap_cjk(wrapped, sizeof(wrapped), text, 28);
    lv_label_set_text(s_dbody, wrapped);
}

static lv_obj_t *s_story;              // 叙事页正文标签(升空/结局;M5)

// 叙事页正文:198px 内容区,32 半角格折行(与弹窗同款预折行路径)
static void set_story(const char *text) {
    static char wrapped[320];
    wrap_cjk(wrapped, sizeof(wrapped), text, 32);
    lv_label_set_text(s_story, wrapped);
    lv_obj_clear_flag(s_story, LV_OBJ_FLAG_HIDDEN);
}

// ===================================================================
// build_ui:一次性创建全部对象(含隐藏),页面渲染只改属性与可见性
// ===================================================================
static lv_obj_t *s_t_big, *s_t_sub;    // 标题页大标题/副文案(专用)
static lv_obj_t *s_t_ok;
static lv_obj_t *s_t_rule, *s_t_fire;  // 顶饰线+火光带(美化 T3)
static uint32_t s_title_reveal_ms;     // 副标题打字机基准(0=未开始)

// 地形纹样描述符(定义见文件后部,图例在此先取址)
static const lv_image_dsc_t k_pat_dots, k_pat_hstrip, k_pat_diag;

// I33:幽灵键窗锚点挂"flush 完成"——真正的电流尖峰在 esp_lvgl_port 任务的
// 刷新/DMA 里,重负载页可拖到布局写完 60ms 之后,只锚 render() 会漏。
// 回调运行于持同一把 LVGL 锁的移植层任务,uint32 写读无需额外同步。
static void on_flush_finish(lv_event_t *e) {
    (void)e;
    s.last_render_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

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

    // 页头规则线+accent 短色条(T2):最先创建=最底层,压在日志/列表行之下;
    // 位置与显隐由各页渲染时全量重写(clear_scene 统一藏)
    s_rule = lv_obj_create(s_content);
    lv_obj_set_pos(s_rule, 0, 44);
    lv_obj_set_size(s_rule, 198, 1);
    lv_obj_set_style_radius(s_rule, 0, 0);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_border_width(s_rule, 0, 0);
    lv_obj_set_style_pad_all(s_rule, 0, 0);
    lv_obj_clear_flag(s_rule, LV_OBJ_FLAG_SCROLLABLE);
    s_tick = lv_obj_create(s_content);
    lv_obj_set_pos(s_tick, 0, 44);
    lv_obj_set_size(s_tick, 26, 1);
    lv_obj_set_style_radius(s_tick, 0, 0);
    lv_obj_set_style_bg_color(s_tick, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(s_tick, 0, 0);
    lv_obj_set_style_pad_all(s_tick, 0, 0);
    lv_obj_clear_flag(s_tick, LV_OBJ_FLAG_SCROLLABLE);

    // 内嵌面板(T5,最底层):主页日志区/村庄职业窗的"叙事底板"——
    // 深一档底色+1px 分隔线框,把叙事与操作分开;位置尺寸各页渲染时定
    s_logpanel = lv_obj_create(s_content);
    lv_obj_set_pos(s_logpanel, 0, 86);
    lv_obj_set_size(s_logpanel, 198, 140);
    lv_obj_set_style_radius(s_logpanel, 3, 0);
    lv_obj_set_style_bg_color(s_logpanel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(s_logpanel, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_border_width(s_logpanel, 1, 0);
    lv_obj_set_style_pad_all(s_logpanel, 0, 0);
    lv_obj_clear_flag(s_logpanel, LV_OBJ_FLAG_SCROLLABLE);
    s_jobpanel = lv_obj_create(s_content);
    lv_obj_set_pos(s_jobpanel, 0, 117);
    lv_obj_set_size(s_jobpanel, 192, 118);
    lv_obj_set_style_radius(s_jobpanel, 3, 0);
    lv_obj_set_style_bg_color(s_jobpanel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(s_jobpanel, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_border_width(s_jobpanel, 1, 0);
    lv_obj_set_style_pad_all(s_jobpanel, 0, 0);
    lv_obj_clear_flag(s_jobpanel, LV_OBJ_FLAG_SCROLLABLE);
    s_sep = lv_obj_create(s_content);
    lv_obj_set_pos(s_sep, 0, 75);
    lv_obj_set_size(s_sep, 198, 1);
    lv_obj_set_style_radius(s_sep, 0, 0);
    lv_obj_set_style_bg_color(s_sep, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_border_width(s_sep, 0, 0);
    lv_obj_set_style_pad_all(s_sep, 0, 0);
    lv_obj_clear_flag(s_sep, LV_OBJ_FLAG_SCROLLABLE);
    s_logtick = lv_obj_create(s_content);
    lv_obj_set_pos(s_logtick, 1, 92);
    lv_obj_set_size(s_logtick, 2, 11);
    lv_obj_set_style_radius(s_logtick, 1, 0);
    lv_obj_set_style_bg_color(s_logtick, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_border_width(s_logtick, 0, 0);
    lv_obj_set_style_pad_all(s_logtick, 0, 0);
    lv_obj_clear_flag(s_logtick, LV_OBJ_FLAG_SCROLLABLE);
    // 共享焦点条(T7d):底层透显——行容器全透明底,金条从下方露出,
    // 行文字(晚创建=高层)压在金条上;位置由 render() 尾统一驱动
    s_focusbar = lv_obj_create(s_content);
    lv_obj_set_size(s_focusbar, 198, 18);
    lv_obj_set_style_radius(s_focusbar, 3, 0);
    lv_obj_set_style_bg_color(s_focusbar, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(s_focusbar, 0, 0);
    lv_obj_set_style_pad_all(s_focusbar, 0, 0);
    lv_obj_clear_flag(s_focusbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_focusbar, LV_OBJ_FLAG_HIDDEN);

    // 顶栏:标题(0,0,110,20 f12) + 电池(150,2,48,15 f12 右对齐)
    s_topbar = label_new(s_content, &dr_font_12, lv_color_hex(COL_GOLD), 0, 0, 110, 20);
    lv_label_set_text(s_topbar, "小黑屋");
    s_batt = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 150, 2, 48, 15);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_batt, "--%");

    // 场景导航栏 5×38×20 @ y=24(步进 40;M5 星舰页签),文字 + 底部指示线
    static const char *tab_names[5] = {"小屋", "村庄", "荒野", "星舰", "≡"};
    for (int i = 0; i < 5; i++) {
        s_tabs[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_tabs[i], i * 40, 24);
        lv_obj_set_size(s_tabs[i], 38, 20);
        lv_obj_set_style_bg_opa(s_tabs[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tabs[i], 0, 0);
        lv_obj_set_style_border_side(s_tabs[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_tabs[i], 1, 0);
        lv_obj_set_style_border_color(s_tabs[i], lv_color_hex(COL_CD), 0);
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
                                  lv_color_hex(COL_DIM), 0, 90 + i * 15, 194, 15);
        lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 页面底部说明行
    s_hint = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 0, 256, 198, 15);

    // 滚动指示条(轨道暗/滑块亮):位置与长度由村庄/制造页渲染时全量重写
    for (int i = 0; i < 2; i++) {
        lv_obj_t **p = i ? &s_sthumb : &s_strack;
        *p = lv_obj_create(s_content);
        lv_obj_set_pos(*p, 195, 0);
        lv_obj_set_size(*p, 2, 8);
        lv_obj_set_style_radius(*p, 1, 0);
        lv_obj_set_style_bg_color(*p,
            lv_color_hex(i ? COL_DIM : COL_CD), 0);
        lv_obj_set_style_border_width(*p, 0, 0);
        lv_obj_set_style_pad_all(*p, 0, 0);
        lv_obj_clear_flag(*p, LV_OBJ_FLAG_SCROLLABLE);
    }

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
        a->arrow = label_new(a->row, &dr_font_12, lv_color_hex(COL_GOLD),
                             2, 1, ARROW_W12, 20);
        a->lbl = label_new(a->row, &dr_font_12, lv_color_hex(COL_TEXT),
                           2 + ARROW_W12, 1, 186 - ARROW_W12 - 4, 20);
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
        // t 列 58px(最长实文"械库 Lv10"≈50px);v 列 120px 右对齐
        // ("1500木100铁100煤"=103px,90px 时代必截断,I4)
        r->t = label_new(r->row, &dr_font_12, lv_color_hex(COL_TEXT),
                         4 + ARROW_W12, 1, 58, 20);
        r->v = label_new(r->row, &dr_font_12, lv_color_hex(COL_DIM), 72, 1, 120, 20);
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
    s_dbody = label_new(s_dpanel, &dr_font_12, lv_color_hex(COL_TEXT), 0, 0, 176, 100);

    // 弹窗选项行 5 行 172 宽(页面渲染时定位;M6 切片二 4→5)
    for (int i = 0; i < 5; i++) {
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
        a->arrow = label_new(a->row, &dr_font_12, lv_color_hex(COL_GOLD),
                             2, 1, ARROW_W12, 20);
        a->lbl = label_new(a->row, &dr_font_12, lv_color_hex(COL_TEXT),
                           2 + ARROW_W12, 1, 160 - ARROW_W12 - 4, 20);
    }

    // 叙事页正文(升空/结局共用;M5):整页仅此一个长文标签,各页渲染时全量重写
    s_story = label_new(s_content, &dr_font_12, lv_color_hex(COL_TEXT),
                        0, 48, 198, 170);
    lv_obj_add_flag(s_story, LV_OBJ_FLAG_HIDDEN);

    // 存档码页(M7):页码行 + 6 字符大间隔;导入 3 行回看。常驻对象 10 枚
    // f12 label(~1.5KB LVGL 池,内存红线内的克制增量)。
    s_code_hdr = label_new(s_content, &dr_font_12, lv_color_hex(COL_GOLD),
                           0, 30, 198, 15);
    lv_obj_set_style_text_align(s_code_hdr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_code_hdr, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < CODE_COLS; i++) {
        s_code_char[i] = label_new(s_content, &dr_font_12,
                                   lv_color_hex(COL_TEXT), 8 + i * 32, 120,
                                   30, 15);
        lv_obj_set_style_text_align(s_code_char[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_code_char[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        s_imp_lines[i] = label_new(s_content, &dr_font_12,
                                   lv_color_hex(COL_TEXT), 0, 70 + i * 22,
                                   198, 15);
        lv_obj_set_style_text_align(s_imp_lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_imp_lines[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 标题页三件套(独立对象;组块中心 ~139 垂直居中)
    s_t_big = label_new(s_content, &dr_font_24, lv_color_hex(COL_GOLD), 0, 88, 200, 30);
    lv_obj_set_style_text_align(s_t_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_t_big, "小黑屋");
    s_t_sub = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM), 0, 124, 200, 15);
    lv_obj_set_style_text_align(s_t_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_t_sub, "屋里冷得刺骨");
    s_t_ok = lv_obj_create(s_content);
    lv_obj_set_pos(s_t_ok, 40, 168);
    lv_obj_set_size(s_t_ok, 120, ROW_H);
    lv_obj_set_style_radius(s_t_ok, 3, 0);
    lv_obj_set_style_bg_opa(s_t_ok, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_t_ok, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(s_t_ok, 1, 0);
    lv_obj_set_style_pad_all(s_t_ok, 0, 0);
    lv_obj_clear_flag(s_t_ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tok = label_new(s_t_ok, &dr_font_12, lv_color_hex(COL_GOLD), 0, 1, 120, 20);
    lv_obj_set_style_text_align(tok, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(tok, "OK键开始");
    // 顶饰线+火光带(美化 T3):装饰暗于金;火光带=底部余烬(两段渐变,
    // 顶部全透明→底部 ~35% 暖橙,主/渐同色仅透明度差)
    s_t_rule = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM),
                         0, 72, 200, 14);
    lv_obj_set_style_text_align(s_t_rule, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_t_rule, "───── · ─────");
    s_t_fire = lv_obj_create(s_content);
    lv_obj_set_pos(s_t_fire, 0, 254);
    lv_obj_set_size(s_t_fire, 198, 24);
    lv_obj_set_style_radius(s_t_fire, 2, 0);
    lv_obj_set_style_border_width(s_t_fire, 0, 0);
    lv_obj_set_style_pad_all(s_t_fire, 0, 0);
    lv_obj_set_style_bg_color(s_t_fire, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_grad_color(s_t_fire, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_main_opa(s_t_fire, 0, 0);
    lv_obj_set_style_bg_grad_opa(s_t_fire, 89, 0);
    lv_obj_set_style_bg_grad_dir(s_t_fire, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(s_t_fire, LV_OBJ_FLAG_SCROLLABLE);


    // 地图 9×9 格 @(14,44) 格10px 间隔1(左移腾出右侧全量图例双列)
    for (int i = 0; i < MAP_CELLS; i++) {
        s_map[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_map[i], 14 + (i % 9) * 11, 44 + (i / 9) * 11);
        lv_obj_set_size(s_map[i], 10, 10);
        lv_obj_set_style_radius(s_map[i], 0, 0);
        lv_obj_set_style_border_width(s_map[i], 1, 0);
        lv_obj_set_style_border_color(s_map[i], lv_color_hex(COL_RULE), 0);
        lv_obj_set_style_bg_color(s_map[i], lv_color_hex(COL_BG), 0);
        lv_obj_set_style_pad_all(s_map[i], 0, 0);
    }
    // 全量图例(远征态,地图右侧双列 9 行):色块+短名,覆盖全部地块色。
    // 左列=地形与核心,右列=矿与地点;三地形色块与地图同款纹样。
    // 18 项后行距 14→12(尾行止 140,仍在地图带内);4 新地标色见 map_tile_color。
    static const struct {
        uint32_t col; const char *name; const lv_image_dsc_t *pat;
    } k_leg[LEG_KINDS] = {
        // 左列(前 9):地形与核心
        { 0x2E5E1E, "森林", &k_pat_dots },  { 0xC8B84B, "田野", &k_pat_hstrip },
        { 0x967850, "荒地", &k_pat_diag },  { COL_GOLD, "村庄", NULL },
        { COL_ME,   "你",   NULL },          { 0x9AE8E8, "哨站", NULL },
        { 0xC060D8, "星舰", NULL },          { 0x2E2A6B, "钻孔", NULL },
        { 0x75465D, "战场", NULL },
        // 右列(后 9):矿与地点
        { 0x707078, "铁矿", NULL },          { 0x26262E, "煤矿", NULL },
        { 0x9B8C34, "硫矿", NULL },          { 0x965F37, "老屋", NULL },
        { 0x4B64BE, "洞穴", NULL },          { 0xF29E9E, "废镇", NULL },
        { 0x5A3A90, "城市", NULL },          { 0x589E8C, "沼泽", NULL },
        { 0x230412, "残村", NULL },
    };
    {
        static const int colx[2] = { 116, 158 };   // 双列 x(列宽 42,尾列止 193)
        for (int i = 0; i < LEG_KINDS; i++) {
            int x = colx[i / 9];
            int y = 44 + (i % 9) * 12;             // 9 行 × 12,与地图带同高
            s_legchip[i] = lv_obj_create(s_content);
            lv_obj_set_pos(s_legchip[i], x, y + 1);
            lv_obj_set_size(s_legchip[i], 10, 10);
            lv_obj_set_style_radius(s_legchip[i], 1, 0);
            lv_obj_set_style_bg_color(s_legchip[i], lv_color_hex(k_leg[i].col), 0);
            lv_obj_set_style_bg_image_src(s_legchip[i], k_leg[i].pat, 0);
            lv_obj_set_style_border_width(s_legchip[i], 0, 0);
            lv_obj_set_style_pad_all(s_legchip[i], 0, 0);
            s_legtxt[i] = label_new(s_content, &dr_font_12, lv_color_hex(COL_DIM),
                                    x + 13, y, 26, 12);
            lv_label_set_text(s_legtxt[i], k_leg[i].name);
        }
    }

    // 房间 3×3 格 @(58,44) 格26px 间隔2
    for (int i = 0; i < ROOM_CELLS; i++) {
        s_room[i] = lv_obj_create(s_content);
        lv_obj_set_pos(s_room[i], 58 + (i % 3) * 28, 44 + (i / 3) * 28);
        lv_obj_set_size(s_room[i], 26, 26);
        lv_obj_set_style_radius(s_room[i], 0, 0);
        lv_obj_set_style_border_width(s_room[i], 1, 0);
        lv_obj_set_style_border_color(s_room[i], lv_color_hex(COL_RULE), 0);
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
    // 血条 25/50/75% 刻度(T4):面板色 1px 分隔,分段条质感;子对象随条显隐
    for (int i = 0; i < 6; i++) {
        s_hptick[i] = lv_obj_create(s_hp[i / 3]);
        lv_obj_set_pos(s_hptick[i], 1 + (196 / 4) * ((i % 3) + 1), 1);
        lv_obj_set_size(s_hptick[i], 1, 8);
        lv_obj_set_style_radius(s_hptick[i], 0, 0);
        lv_obj_set_style_bg_color(s_hptick[i], lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_border_width(s_hptick[i], 0, 0);
        lv_obj_set_style_pad_all(s_hptick[i], 0, 0);
        lv_obj_clear_flag(s_hptick[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_screen_load(s_scr);
    lv_display_add_event_cb(lv_display_get_default(), on_flush_finish,
                            LV_EVENT_FLUSH_FINISH, NULL);
}

// ===================================================================
// 渲染辅助
// ===================================================================
static void render_topbar_c(const char *title, uint32_t accent) {
    lv_label_set_text(s_topbar, title);
    lv_obj_set_style_text_color(s_topbar, lv_color_hex(accent), 0);
}

// 默认金色标题(仪式页:标题/升空/结局豁免 accent)
static void render_topbar(const char *title) {
    render_topbar_c(title, COL_GOLD);
}

// 地点页动态 accent(与地图/图例同源,提亮至文字可读档;T2)
static uint32_t ruin_accent(void) {
    switch (dr_world_location(&s_world)) {
        case DR_WT_HOUSE: return ACCENT_BUILD;
        case DR_WT_CAVE:  return 0x8AA0E8;
        case DR_WT_TOWN:  return 0xF29E9E;
        case DR_WT_CITY:  return 0xA488D8;
        case DR_WT_BOREHOLE: return 0x8D84E8;   // 深靛蓝提亮
        case DR_WT_BFIELD:   return 0xD08CA2;   // 暗锈红提亮
        case DR_WT_SWAMP:    return 0x8FD8C0;   // 青绿提亮
        case DR_WT_RUINV:    return 0xC08A9A;   // 焦黑提亮(余烬色)
        default:          return COL_DIM;
    }
}

// 页头规则线+左端 accent 短色条(T2):仅无页签的列表页,显隐由 clear_scene
// 统一藏、各页按需调用;tick 画在 rule 左端同带(后创建者在上)
static void render_rule(int y, int h, uint32_t accent) {
    lv_obj_clear_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_rule, 0, y);
    lv_obj_set_size(s_rule, 198, h);
    lv_obj_clear_flag(s_tick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_tick, 0, y);
    lv_obj_set_size(s_tick, 26, h);
    lv_obj_set_style_bg_color(s_tick, lv_color_hex(accent), 0);
}

// 页签渲染:active=当前页(金色下划线),nav=光标所在 tab(-1=不在页签区)。
// 焦点(完整金框)与所在页(下划线)是两个独立状态,可同时落在同一 tab。
static void render_tabs(int active, bool village_ok, bool wild_ok, int nav) {
    // 第二页签名随定居进度变化(原版:森林 → 村庄)
    lv_label_set_text(s_tablbl[1], outside_name());
    bool ship_ok = nav_tab_enabled(3);
    for (int i = 0; i < 5; i++) {
        lv_obj_clear_flag(s_tabs[i], LV_OBJ_FLAG_HIDDEN);
        bool on = (i == active);
        bool cur = (i == nav);
        bool lock = (i == 1 && !village_ok) || (i == 2 && !wild_ok) ||
                    (i == 3 && !ship_ok);
        // T1:光标页签=金底墨字色块;锁定光标不反色,保留锁灰描边以示可落
        bool inv = cur && !lock;
        lv_obj_set_style_bg_opa(s_tabs[i],
                                inv ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (inv)
            lv_obj_set_style_bg_color(s_tabs[i], lv_color_hex(COL_GOLD), 0);
        lv_obj_set_style_text_color(s_tablbl[i],
            lv_color_hex(inv ? COL_INK
                         : lock ? COL_LOCK
                         : (on || cur) ? COL_GOLD : COL_DIM), 0);
        lv_obj_set_style_border_color(s_tabs[i],
            lv_color_hex((cur && lock) ? COL_LOCK
                         : (on || cur) ? COL_GOLD : COL_CD), 0);
        lv_obj_set_style_border_width(s_tabs[i],
            on ? 2 : (cur ? 2 : 1), 0);
        lv_obj_set_style_outline_color(s_tabs[i], lv_color_hex(COL_LOCK), 0);
        lv_obj_set_style_outline_width(s_tabs[i],
                                       (cur && lock) ? 1 : 0, 0);
        lv_obj_set_style_outline_pad(s_tabs[i], 0, 0);
    }
}

// 资源格基准排版:46×15 f12。各页显示前必须经此或显式重写,
// 抵消其他页(战斗页拉通 198 宽)留下的几何残留。
static void cell_base(int i, int x, int y) {
    lv_obj_set_pos(s_cells[i], x, y);
    lv_obj_set_size(s_cells[i], 46, 15);
    lv_obj_set_style_text_font(s_cells[i], &dr_font_12, 0);
    lv_obj_set_style_text_color(s_cells[i], lv_color_hex(COL_TEXT), 0);
}

// 资源格文字:"木 12.3k"——数量经紧凑化(46px 格 5 位数起必截断,I1)
static void cell_res(int i, const char *label, uint32_t v) {
    char num[8];
    dr_fmt_compact(num, sizeof(num), v);
    lv_label_set_text_fmt(s_cells[i], "%s %s", label, num);
}

static void render_cells(void) {
    for (int i = 0; i < 8; i++) {
        cell_base(i, (i % 4) * 49, 48 + (i / 4) * 15);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(s_cells[0], "火%s", fire_char());
    cell_res(1, "人", s.game.population);
    cell_res(2, "阱", s.game.building_lv[DR_BLD_TRAP]);
    cell_res(3, "木", s.game.res[DR_RES_WOOD]);
    // 木=0 → 点不着火,直接原因上屏(T4,同远征断供红格先例)
    lv_obj_set_style_text_color(s_cells[3],
        lv_color_hex(s.game.res[DR_RES_WOOD] ? COL_TEXT : COL_ENEMY), 0);
    cell_res(4, "毛", s.game.res[DR_RES_FUR]);
    cell_res(5, "肉", s.game.res[DR_RES_MEAT]);
    cell_res(6, "诱", s.game.res[DR_RES_BAIT]);
    cell_res(7, "革", s.game.res[DR_RES_LEATHER]);
}

// 主页日志:逐行标签,最新在上,旧条目颜色渐隐到背景(web 版 fade)。
// 行数随动作行数自适应:3 个动作时更多行,4 个及以上时减少一行。
static lv_obj_t *s_loglines[LOG_VIS];

// 第 k 新条目的语义基色(T4):我=蓝/敌=红/系统=白
static uint32_t log_base(int k) {
    int idx = s_log_cnt - 1 - k;
    uint8_t o = (idx >= 0) ? s_logs_origin[idx] : (uint8_t)LOG_SYS;
    return (o == LOG_ME) ? COL_ME : (o == LOG_FOE) ? COL_ENEMY : COL_TEXT;
}

// base 色向 COL_BG 渐隐(i=0 最新全亮,越旧越融入背景 ~67%)
static lv_color_t log_fade(int i, int n, uint32_t base) {
    int t = (n <= 1) ? 0 : (i * 170) / (n - 1);
    uint32_t r = ((base >> 16) & 0xFF) * (255 - t) / 255 +
                 ((COL_BG >> 16) & 0xFF) * t / 255;
    uint32_t g = ((base >> 8) & 0xFF) * (255 - t) / 255 +
                 ((COL_BG >> 8) & 0xFF) * t / 255;
    uint32_t b = (base & 0xFF) * (255 - t) / 255 +
                 (COL_BG & 0xFF) * t / 255;
    return lv_color_hex((r << 16) | (g << 8) | b);
}

// 取第 k 新的条目(k=0 最新);没有则返回 NULL
static const char *log_entry_newest_first(int k) {
    int idx = s_log_cnt - 1 - k;
    return (idx >= 0) ? s_logs[idx] : NULL;
}

// 主页日志渲染:n_acts = 本页动作行数,act_top = 首行动作行 y(动作行底部对齐,
// 行数少时日志区自动加高,上限 LOG_VIS)
static void render_home_log(int n_acts, int act_top) {
    (void)n_acts;
    const int pitch = 15;
    int max_lines = (act_top - 90) / pitch;
    int show = max_lines;
    if (show > LOG_VIS) show = LOG_VIS;
    // 日志内嵌面板(T5):叙事区与操作区分隔,随动作行数自适应
    lv_obj_clear_flag(s_logpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_logpanel, 0, 86);
    lv_obj_set_size(s_logpanel, 198, act_top - 90);
    // 浏览窗(T8):选中态 off 生效,窗口滑向更早记录;聚焦/普通态 off=0
    int off = s.log_browse ? s.log_off : 0;
    int max_off = (s_log_cnt > LOG_VIS) ? s_log_cnt - LOG_VIS : 0;
    if (off > max_off) off = max_off;
    for (int i = 0; i < LOG_VIS; i++) {
        int a = off + i;   // 绝对序(0=最新),越窗即旧
        const char *e = (a < s_log_cnt) ? s_logs[s_log_cnt - 1 - a] : NULL;
        lv_obj_t *l = s_loglines[i];
        if (i >= show || !e) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_pos(l, 4, 90 + i * pitch);   // 面板内缩进 4px
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(l, e);
        lv_obj_set_style_text_color(l, log_fade(a, s_log_cnt, log_base(a)), 0);
    }
    // 日志窗聚焦/选中态(T8):面板边框金描边(选中=金+右缘滚动条)
    lv_obj_set_style_border_color(s_logpanel,
        lv_color_hex((s.log_focus || s.log_browse) ? COL_GOLD : COL_RULE), 0);
    // 最新一行锚点条(T7e):静止(未聚焦/未选中)且有日志时点亮
    if (show > 0 && s_log_cnt > 0 && !s.log_focus && !s.log_browse) {
        lv_obj_clear_flag(s_logtick, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_logtick, 1, 92);
    }
    // 选中态滚动指示条(T8):复用轨道/滑块,置于面板右缘
    if (s.log_browse && s_log_cnt > LOG_VIS && act_top - 94 > 12) {
        int tr_h = act_top - 94;
        int th = tr_h * LOG_VIS / s_log_cnt;
        if (th < 6) th = 6;
        lv_obj_set_pos(s_strack, 194, 90);
        lv_obj_set_size(s_strack, 2, tr_h);
        lv_obj_clear_flag(s_strack, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_sthumb, 194, 90 + off * (tr_h - th) / max_off);
        lv_obj_set_size(s_sthumb, 2, th);
        lv_obj_clear_flag(s_sthumb, LV_OBJ_FLAG_HIDDEN);
    }
}

// 动作行:文字 + 焦点 + 冷却填充百分比(0=就绪无底色)。
// 焦点态(T1/T7d):反色金块由共享焦点条承担(set_act 只染墨字/下移文字);
// 冷却中退为 1px 金描边;对话框选项行(异父)保持逐行反色,不接共享条
static void set_act(actrow_t *a, const char *text, bool focus, int fill_pct) {
    lv_label_set_text(a->arrow, focus ? ">" : "");
    lv_label_set_text(a->lbl, text);
    bool inv = focus && fill_pct <= 0;
    bool own_bar = inv && lv_obj_get_parent(a->row) == s_content;
    if (inv && !own_bar) {
        lv_obj_set_style_bg_opa(a->row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(a->row, lv_color_hex(COL_GOLD), 0);
    } else {
        lv_obj_set_style_bg_opa(a->row, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_border_width(a->row, (focus && !inv) ? 1 : 0, 0);
    if (focus && !inv)
        lv_obj_set_style_border_color(a->row, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_color(a->arrow,
                                lv_color_hex(inv ? COL_INK : COL_GOLD), 0);
    lv_obj_set_style_text_color(a->lbl,
        lv_color_hex(inv ? COL_INK : focus ? COL_GOLD : COL_TEXT), 0);
    int ty = inv ? 3 : 1;
    lv_obj_set_pos(a->arrow, 2, ty);
    lv_obj_set_pos(a->lbl, 2 + ARROW_W12, ty);
    if (own_bar) {
        s_bar_act = a;
        s_bar_row = -1;
    }
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

// 列表行:t 名称(focus 反色墨字/lock 灰) v 值(右对齐)。
// T1:焦点整行反色金底墨字;锁定行永不反色。
// T7d:反色金块由共享焦点条承担,行容器恒 22 透明底
static void set_row(int i, int y, const char *t, const char *v,
                    bool focus, bool lock) {
    listrow_t *r = &s_list[i];
    lv_obj_set_pos(r->row, 0, y);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    bool inv = focus && !lock;
    if (inv) {
        s_bar_act = NULL;
        s_bar_row = i;
    }
    int ty = inv ? 3 : 1;
    lv_label_set_text(r->mark, focus ? ">" : "");
    lv_obj_set_pos(r->mark, 4, ty);
    lv_obj_set_style_text_color(r->mark,
                                lv_color_hex(inv ? COL_INK : COL_GOLD), 0);
    lv_label_set_text(r->t, t);
    lv_obj_set_pos(r->t, 4 + ARROW_W12, ty);
    lv_obj_set_style_text_color(r->t,
        lv_color_hex(inv ? COL_INK
                         : focus ? COL_GOLD : lock ? COL_LOCK : COL_TEXT), 0);
    lv_label_set_text(r->v, v);
    lv_obj_set_pos(r->v, 72, ty);
    lv_obj_set_style_text_color(r->v,
        lv_color_hex(inv ? COL_INK : lock ? COL_LOCK : COL_DIM), 0);
}

static void hide_rows(void) {
    for (int i = 0; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
}

// 每页渲染前统一藏起全部场景件,再由各页按需显示
static void clear_scene(void) {
    hide_acts();
    hide_rows();
    for (int i = 0; i < 5; i++) lv_obj_add_flag(s_tabs[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_logpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_jobpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_logtick, LV_OBJ_FLAG_HIDDEN);
    // 焦点条(T7d)不在此隐藏/重置:它的显隐与归属由 render() 尾全权驱动,
    // 页面存续期的补间不能被中间渲染打断;切页时由 page_goto 统一收场
    lv_obj_add_flag(s_strack, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sthumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_ok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_t_fire, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_story, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAP_CELLS; i++) lv_obj_add_flag(s_map[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < LEG_KINDS; i++) {
        lv_obj_add_flag(s_legchip[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_legtxt[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < ROOM_CELLS; i++) lv_obj_add_flag(s_room[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) lv_obj_add_flag(s_hp[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_code_hdr, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < CODE_COLS; i++)
        lv_obj_add_flag(s_code_char[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++)
        lv_obj_add_flag(s_imp_lines[i], LV_OBJ_FLAG_HIDDEN);
    if (!s_veil_fading)   // 淡入进行中 veil 由动画接管(clear_scene 让路,T7a)
        lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 5; i++) lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
}

// ===================================================================
// 电源(DESIGN §8.1/§8.2):L1 熄屏 + L2 浅睡 + L3 深睡 + 电量保护
// ===================================================================
// L1 档位 0..4:关/30秒/1分/2分/5分。_rsv[1] 位打包:低 3 位 = L1 档
// (老档 0..4 零迁移),位 3..5 = L2/L3 睡眠模式(0=关 1=浅睡 2=深睡
// 3=浅+深;老档高位为零=关,同样零迁移)。
// L2 打盹:三键是 ADC 分压(共用 GPIO0),浅睡中 ADC 无法采样、GPIO 唤醒
// 不可用 —— 用 1s RTC 定时器打盹循环替代(醒来查队列/ADC,有键即退;
// 省电近似浅睡底电流)。L3 深睡:松开态引脚上拉 3300mV、按下最高 595mV
// (< 数字低电平),GPIO0 低电平唤醒三键全可用;唤醒=重启走离线结算。
// 手动:双击下键熄屏(L1);唤醒键一律吞掉(防误触)。战斗/事件/确认
// 弹屏挂起自动计时(§8.1);电量≤10% 硬保护不受挂起限制(§8.2)。
#define DR_BL_ON 100
#define DR_BL_DIM 50                 // 电量≤20% 背光减半(§8.2)
#define DR_LIGHT_SLEEP_S 60u         // L2 门槛(可配)
#define DR_DEEP_SLEEP_S 300u         // L3 门槛(可配)
#define DR_BATT_LOW_PCT 20           // ≤20% 降背光+提示
#define DR_BATT_CRIT_PCT 10          // ≤10% 存档+立即深睡
#define DR_BTN_GPIO 0                // 三键分压节点 = GPIO0(bsp_pins.h ADC1_CH0)
static const uint16_t k_scr_off_s[5] = { 0, 30, 60, 120, 300 };
static const char *const k_scr_off_lbl[5] = { "关", "30秒", "1分", "2分", "5分" };
static const char *const k_sleep_lbl[4] = { "关", "浅睡", "深睡", "浅睡+深睡" };
static uint8_t scr_off_level(void) {
    uint8_t v = s.game._rsv[1] & 0x07u;
    return (v > 4) ? 4 : v;
}
static uint8_t sleep_mode(void) {
    return (s.game._rsv[1] >> 3) & 0x07u;
}
// 背光唯一写入口:熄屏/低电降档统一折算(电量为 0 时 LEDC 直接熄)
static void apply_backlight(void) {
    if (s.screen_off) { bsp_display_backlight(0); return; }
    bsp_display_backlight(s.bl_dim ? DR_BL_DIM : DR_BL_ON);
}
static void screen_off_now(void) {
    s.screen_off = true;
    apply_backlight();
}
static void screen_wake(void) {
    s.screen_off = false;
    apply_backlight();
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
    render_topbar_c("小黑屋", ACCENT_HOME);
    render_tabs(0, forest_open(), nav_tab_enabled(2), s.nav_focus);
    render_cells();
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    int n = home_lines();
    // 动作行底部对齐(尾行止于 278):早期 2 行时不再在页底留 48px 空白,
    // 日志区随之加高(render_home_log 按首行 y 自适应行数)
    int gy = 278 - n * 24;
    render_home_log(n, gy);
    for (int i = 0; i < n; i++) {
        lv_obj_set_size(s_acts[i].row, ROW_W, ROW_H);
        lv_obj_set_width(s_acts[i].lbl, 170);
        lv_obj_set_style_text_align(s_acts[i].lbl, LV_TEXT_ALIGN_LEFT, 0);
    }
    int idx = 0;
    bool act_focus = (s.nav_focus < 0) && !s.log_focus;   // 页签/日志窗聚焦时动作区去焦点(T8)
    bool no_wood = s.game.res[DR_RES_WOOD] < DR_FIRE_LIGHT_COST;
    int sp = stoke_pct(now_ms);
    char stbuf[24];
    const char *stoke;
    long stoke_rem_s = -1;
    if (sp > 0) {   // 冷却剩余秒数就地可见(UX:原来只有暗条无数字)
        stoke_rem_s = (long)((s.rules_rt.stoke_ready_ms - now_ms + 999) / 1000);
        snprintf(stbuf, sizeof(stbuf), "%s%lds",
                 s.game.fire_lv == DR_FIRE_DEAD ? "点火中·" : "添柴中·",
                 stoke_rem_s);
        stoke = stbuf;
    } else {
        stoke = (s.game.fire_lv == DR_FIRE_DEAD)
                    ? (no_wood ? "点火(缺木)" : "点火(5木)")
                    : "添柴(1木)";
    }
    set_act(&s_acts[idx], stoke, act_focus && s.focus == idx, sp);
    // E3:冷却末 2s 暗条变琥珀(就绪前提示);其余拍保持暗条本色
    lv_obj_set_style_bg_color(s_acts[idx].fill,
        lv_color_hex((sp > 0 && stoke_rem_s >= 0 && stoke_rem_s <= 2)
                         ? COL_AMBER : COL_CD), 0);
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
        case DR_RES_LEATHER:  return c->leather;   // I29:漏这两个分支时
        case DR_RES_SCALES:   return c->scales;    // 工坊全价只显"800木"、缺口只剩光杆"缺"
        case DR_RES_IRON:     return c->iron;
        case DR_RES_COAL:     return c->coal;
        case DR_RES_STEEL:    return c->steel;
        case DR_RES_SULPHUR:  return c->sulphur;
        default:              return 0;
    }
}

static void render_build(void) {
    render_topbar_c("建造", ACCENT_BUILD);
    render_rule(44, 1, ACCENT_BUILD);
    // 资源摘要单行 @28:木/毛/肉/诱
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 28);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_res(0, "木", s.game.res[DR_RES_WOOD]);
    cell_res(1, "毛", s.game.res[DR_RES_FUR]);
    cell_res(2, "肉", s.game.res[DR_RES_MEAT]);
    cell_res(3, "诱", s.game.res[DR_RES_BAIT]);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    // 列表 8 建筑 + 返回,行距 23 从 52 起
    for (int i = 0; i < DR_BLD_KIND_COUNT; i++) {
        static const struct { uint8_t res; const char *label; } k_comps[] = {
            { DR_RES_FUR, "毛" },     { DR_RES_MEAT, "肉" },
            { DR_RES_LEATHER, "革" }, { DR_RES_SCALES, "鳞" },
            { DR_RES_IRON, "铁" },    { DR_RES_COAL, "煤" },
            { DR_RES_STEEL, "钢" },   { DR_RES_SULPHUR, "硫" },
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
            for (int k = 0; k < 8 && n > 0 && n < (int)sizeof(v); k++) {
                uint32_t cv = bld_comp_cost(k_comps[k].res, &c);
                if (cv) n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                      (unsigned long)cv, k_comps[k].label);
            }
        } else {
            // 不可选:列缺口;无缺口则说明是室温或解锁问题
            int n = 0;
            uint32_t dw = (c.wood > s.game.res[DR_RES_WOOD])
                              ? c.wood - s.game.res[DR_RES_WOOD] : 0;
            if (dw || c.fur || c.meat || c.iron || c.coal || c.steel ||
                c.sulphur || c.leather || c.scales)
                n = snprintf(v, sizeof(v), "缺");
            if (dw) n += snprintf(v + n, sizeof(v) - n, "%lu木", (unsigned long)dw);
            for (int k = 0; k < 8 && n > 0 && n < (int)sizeof(v); k++) {
                uint32_t cv = bld_comp_cost(k_comps[k].res, &c);
                if (!cv) continue;
                uint32_t have = s.game.res[k_comps[k].res];
                if (cv > have)
                    n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                  (unsigned long)(cv - have), k_comps[k].label);
            }
            if (n == 0) snprintf(v, sizeof(v), "%s", warm ? "未解锁" : "太冷");
        }
        // 行起点 46:资源带止于 43,留 3px 间隙(38 起会与资源格叠 5px)
        set_row(i, 46 + i * 19, t, v, s.focus == i,
                !(can && build_affordable((uint8_t)i) && warm));
    }
    set_row(DR_BLD_KIND_COUNT, 46 + DR_BLD_KIND_COUNT * 19, "返回", "",
            s.focus == DR_BLD_KIND_COUNT, false);
    // 底部说明:光标所选建筑的作用
    static const char *bld_desc[] = {
        "采集木材 +50",            // 板车(原版 carry more wood)
        "每 90s 收猎物,可叠 10",   // 陷阱
        "每屋 +4 人口",            // 小屋
        "解锁猎人/捕兽人",          // 猎人小屋
        "游牧商人(只买)",          // 贸易站
        "解锁制革:毛→革",          // 制革坊
        "解锁熏肉:肉→干肉",        // 熏肉房
        "解锁炼钢:铁+煤→钢",       // 炼钢厂
        "解锁军械:钢+硫→弹",       // 军械库
        "解锁制造页(武器护甲)",    // 工坊
    };
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 262);   // 返回行(236-258)之下;圆屏内可见
    if (s.game.temp_lv <= DR_TEMP_COLD) {
        if (s.game.fire_lv == DR_FIRE_DEAD)
            lv_label_set_text(s_hint, "屋里太冷,先点火再建造");
        else {
            char warm[24];
            static const char *tn[5] = { "冻结", "冷", "微温", "暖", "热" };
            snprintf(warm, sizeof(warm), "回暖中:%s→微温", tn[s.game.temp_lv]);
            lv_label_set_text(s_hint, warm);
        }
    }
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
    render_topbar_c(outside_name(), ACCENT_FOREST);
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
    // 职业窗内嵌面板(T5):罩住 5 行窗,右缘让开滚动轨道
    lv_obj_clear_flag(s_jobpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_jobpanel, 0, 117);
    lv_obj_set_size(s_jobpanel, 192, 118);
    set_row(8, 236, adj ? "完成调节" : "返回", "", s.focus == 8, false);

    // 滚动指示条:9 职业装不进 5 行窗,右缘轨道+滑块标示窗口位置(I22)
    lv_obj_set_pos(s_strack, 195, 121);
    lv_obj_set_size(s_strack, 2, 114);
    lv_obj_clear_flag(s_strack, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_sthumb, 195, 121 + (int)s.job_scroll * 51 / 4);
    lv_obj_set_size(s_sthumb, 2, 63);
    lv_obj_clear_flag(s_sthumb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 264);   // 返回行(236)之下,不再遮挡
    if (adj) {
        lv_label_set_text(s_hint, "上加 下减(可长按连发) 确定=完成");
    } else if (s.focus >= 3 && s.focus <= 7 &&
               village_row_job(s.focus) >= 0 &&
               dr_rules_job_unlocked(&s.game,
                                     (uint8_t)village_row_job(s.focus))) {
        // 光标停在职业行:显示该职业的产出配方(同建造页提示模式)
        lv_label_set_text(s_hint, job_desc[village_row_job(s.focus)]);
    } else {
        // 概览:陷阱副产物与干肉(资源格放不下的那几样;紧凑化后四项 179px 放得下。
        // 人口/闲人不再放这里——采集者行已显示闲人数)
        char c1[8], c2[8], c3[8], c4[8];
        dr_fmt_compact(c1, sizeof(c1), s.game.res[DR_RES_SCALES]);
        dr_fmt_compact(c2, sizeof(c2), s.game.res[DR_RES_TEETH]);
        dr_fmt_compact(c3, sizeof(c3), s.game.res[DR_RES_CLOTH]);
        dr_fmt_compact(c4, sizeof(c4), s.game.res[DR_RES_FOOD]);
        char hbuf[64];
        snprintf(hbuf, sizeof(hbuf), "鳞%s 牙%s 布%s 干%s", c1, c2, c3, c4);
        lv_label_set_text(s_hint, hbuf);
    }
}

// 地形/地标配色(荒野视口)
// 地形纹样(色弱第二编码,10×10 透明底覆盖层):森林=白点纹(暗底)、
// 田野=黑横纹(亮底)、荒地=黑斜纹。叠加在地块底色上,图例色块同款。
static const uint32_t k_pat_DOTS_px[100] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000,
    0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000,
    0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x59FFFFFF, 0x59FFFFFF, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t k_pat_HSTRIP_px[100] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000,
    0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000,
    0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000, 0x73000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t k_pat_DIAG_px[100] = {
    0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000,
    0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x73000000,
};
static const lv_image_dsc_t k_pat_dots = {
    .header = { .w = 10, .h = 10, .cf = LV_COLOR_FORMAT_ARGB8888 },
    .data = (const uint8_t *)k_pat_DOTS_px,
    .data_size = sizeof(k_pat_DOTS_px),
};
static const lv_image_dsc_t k_pat_hstrip = {
    .header = { .w = 10, .h = 10, .cf = LV_COLOR_FORMAT_ARGB8888 },
    .data = (const uint8_t *)k_pat_HSTRIP_px,
    .data_size = sizeof(k_pat_HSTRIP_px),
};
static const lv_image_dsc_t k_pat_diag = {
    .header = { .w = 10, .h = 10, .cf = LV_COLOR_FORMAT_ARGB8888 },
    .data = (const uint8_t *)k_pat_DIAG_px,
    .data_size = sizeof(k_pat_DIAG_px),
};
// 地形纹样查找(色弱第二编码):森林=点纹/田野=横纹/荒地=斜纹,其余素色
static const lv_image_dsc_t *tile_pat(uint8_t t) {
    switch ((dr_world_tile_t)t) {
        case DR_WT_FOREST: return &k_pat_dots;
        case DR_WT_FIELD:  return &k_pat_hstrip;
        case DR_WT_BARREN: return &k_pat_diag;
        default:           return NULL;
    }
}

static uint32_t map_tile_color(uint8_t t, bool seen) {
    if (!seen) return COL_BG;                 // 迷雾
    // 色弱适配(2026-10-05):全部色值经绿色盲模拟复核,易混组拉开亮度档
    // (煤矿38/城市72/老屋107/铁矿112/硫矿137/你162/废镇180/村庄·哨站210);
    // 三地形另叠纹样(点/横线/斜线)做第二编码,见 k_pat_*。
    // M7 回补 4 色(2026-10-11 Machado 矩阵核验,与全部既有色两两
    // 模拟距离≥60 或 ΔL≥25):钻孔48/战场84/沼泽138(与硫矿色相远)/残村13。
    switch ((dr_world_tile_t)t) {
        case DR_WT_VILLAGE:   return COL_GOLD;
        case DR_WT_FOREST:    return 0x2E5E1E;
        case DR_WT_FIELD:     return 0xC8B84B;
        case DR_WT_BARREN:    return 0x967850;
        case DR_WT_IRON:      return 0x707078;
        case DR_WT_COAL:      return 0x26262E;
        case DR_WT_SULPHUR:   return 0x9B8C34;
        case DR_WT_OUTPOST:   return 0x9AE8E8;
        case DR_WT_SHIP:      return 0xC060D8;
        case DR_WT_HOUSE:     return 0x965F37;
        case DR_WT_CAVE:      return 0x4B64BE;
        case DR_WT_TOWN:      return 0xF29E9E;
        case DR_WT_CITY:      return 0x5A3A90;
        case DR_WT_BOREHOLE:  return 0x2E2A6B;   // 深靛蓝(原案深灰蓝,让位分档)
        case DR_WT_BFIELD:    return 0x75465D;   // 暗锈红
        case DR_WT_SWAMP:     return 0x589E8C;   // 青绿
        case DR_WT_RUINV:     return 0x230412;   // 焦黑
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
        case DR_WT_BOREHOLE: return "钻孔";
        case DR_WT_BFIELD:   return "战场";
        case DR_WT_SWAMP:    return "沼泽";
        case DR_WT_RUINV:    return "残村";
        default:             return "?";
    }
}

static const char *weapon_name(uint8_t lv) {
    static const char *n[7] = { "拳", "骨矛", "铁剑", "钢剑", "步枪",
                                "刺刀", "激光步枪" };
    return (lv < 7) ? n[lv] : n[0];
}

static const char *armor_name(uint8_t lv) {
    static const char *n[4] = { "无", "皮甲", "铁甲", "钢甲" };
    return (lv < 4) ? n[lv] : n[3];
}

static void render_map(void) {
    render_topbar_c("荒野", ACCENT_WILD);
    // 页签不进地图页(mockup ⑥"荒野地图(无页签)"):资源格才能落在 y24,
    // 不再与页签 y24-44 叠压(I7);长按确定返回小屋
    if (!s.game.in_wilderness) {
        // 出发整备(原版 Path 承重制):干肉/药/子弹在重量预算内自选
        render_rule(44, 2, ACCENT_WILD);
        for (int i = 0; i < 4; i++) {
            cell_base(i, i * 49, 24);
            lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_cells[0], "水 %u", dr_world_water_cap(&s.game));
        lv_label_set_text_fmt(s_cells[1], "袋%u.%u",
                              dr_world_bag_weight(&s.game, &s_world) / 10u,
                              dr_world_bag_weight(&s.game, &s_world) % 10u);
        lv_label_set_text_fmt(s_cells[2], "HP%u", dr_world_health_cap(&s.game));
        lv_label_set_text_fmt(s_cells[3], "承%u",
                              dr_world_bag_cap(&s.game) / 10u);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        char v[24];
        char cbuf[8];   // 库存量紧凑化(可超 5 位)
        bool adj = (s.outfit_adj >= 0);
        // 携带行(库→带):干肉/药/子弹/元件/手雷/流星锤(M5 扩 3→6 行,行距 20)
        static const struct {
            uint8_t res; const char *name; uint16_t (*carry)(const dr_world_t *);
        } k_outfit[6] = {
            { DR_RES_FOOD, "干肉", NULL },
            { DR_RES_MEDICINE, "药", dr_world_carry_medicine },
            { DR_RES_BULLETS, "子弹", dr_world_carry_bullets },
            { DR_RES_ENERGY_CELL, "元件", dr_world_carry_cells },
            { DR_RES_GRENADE, "手雷", dr_world_carry_grenades },
            { DR_RES_BOLAS, "流星锤", dr_world_carry_bolas },
        };
        for (int i = 0; i < 6; i++) {
            uint16_t c = k_outfit[i].carry ? k_outfit[i].carry(&s_world)
                                           : s.game.food;
            dr_fmt_compact(cbuf, sizeof(cbuf), s.game.res[k_outfit[i].res]);
            snprintf(v, sizeof(v), "库%s 带%u", cbuf, c);
            bool none = (s.game.res[k_outfit[i].res] == 0 && c == 0);
            set_row(i, 52 + i * 20, k_outfit[i].name, v, s.focus == i,
                    i != 0 && none);
        }
        // 只读装备行(出发前一目了然;不可聚焦)
        set_row(6, 172, "武器", weapon_name(s.game.weapon_lv), false, true);
        snprintf(v, sizeof(v), "%s HP%u", armor_name(s.game.armor_lv),
                 dr_world_health_cap(&s.game));
        set_row(7, 192, "护甲", v, false, true);
        if ((int32_t)(now_ms - s.embark_cd_ms) < 0)
            snprintf(v, sizeof(v), "%lds",
                     (long)((s.embark_cd_ms - now_ms + 999) / 1000));
        else
            snprintf(v, sizeof(v), "就绪");
        // 冷却中也置灰(仍可聚焦,确定弹"歇一歇"提示)——只按 food 置灰会让
        // 冷却行看似正常却选不中,像死键(真机反馈 2026-10-05)
        set_row(8, 212, "出发远征", v, s.focus == 8,
                s.game.food == 0 ||
                    (int32_t)(now_ms - s.embark_cd_ms) < 0);
        set_row(9, 232, "返回", "", s.focus == 9, false);
        for (int i = 10; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hint, 0, 256);
        if (adj)
            lv_label_set_text(s_hint, "上下调整(可长按连发) 确定完成");
        else if (s.game.food == 0)
            lv_label_set_text(s_hint, "整备干肉后才能出发(库→带)");
        else
            lv_label_set_text(s_hint, "承重:肉药雷1 锤0.5 元0.2 弹0.1");
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
        lv_obj_set_style_bg_image_src(s_map[i],
            (!me && seen) ? tile_pat(t) : NULL, 0);
    }
    // 全量图例(地图右侧双列):颜色→含义钥匙
    for (int i = 0; i < LEG_KINDS; i++) {
        lv_obj_clear_flag(s_legchip[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_legtxt[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "水 %u", s.game.water);
    lv_label_set_text_fmt(s_cells[1], "食 %u", s.game.food);
    lv_label_set_text_fmt(s_cells[2], "HP%u/%u", s.game.hero_hp, s.game.hero_hp_max);
    lv_label_set_text_fmt(s_cells[3], "里 %u", dr_world_home_dist(&s.game));
    // 危险警示:离家太远且无护甲 → "里"格文字变红(原 y306 提示行超出内容区永远不可见,I9)
    lv_obj_set_style_text_color(s_cells[3],
        lv_color_hex(dr_world_danger(&s.game) ? COL_ENEMY : COL_TEXT), 0);
    // 断供警告:断水/断粮第一次警告只进主页日志,荒野页不可见 → 猝死无预感
    // (2026-10-07 B 项)。警告态把"水"/"食"格文字变红,与"里"格同款模式。
    lv_obj_set_style_text_color(s_cells[0],
        lv_color_hex(s_world.warned_thirst ? COL_ENEMY : COL_TEXT), 0);
    lv_obj_set_style_text_color(s_cells[1],
        lv_color_hex(s_world.warned_hunger ? COL_ENEMY : COL_TEXT), 0);

    // 方向行:显示目标格(雾内显示"未知");行距 22,尾行止于 278
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
        set_row(d, 168 + d * 22, dir_names[d], v, s.focus == d, false);
    }
    snprintf(v, sizeof(v), "余%u", s.game.food);
    set_row(4, 168 + 4 * 22, "吃干肉", s.game.food ? v : "没有",
            s.focus == 4, s.game.food == 0);
    for (int i = 5; i < LIST_ROWS; i++) lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    // 危险提示不再单列一行(旧 y285 超出内容区永远不可见,I9):
    // 危险态由"里"格红字承担(见上方资源格)
}

static void render_ruin(void) {
    render_topbar_c(dr_world_location_name(dr_world_location(&s_world)),
                    ruin_accent());
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
    // 房间格阵(44..128):中段 114px 不再留白,中心格 = 当前地点色,搜索点
    for (int i = 0; i < ROOM_CELLS; i++) {
        lv_obj_clear_flag(s_room[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_room[i],
            lv_color_hex(i == 4 ? map_tile_color(dr_world_location(&s_world), true)
                                : COL_PANEL), 0);
    }
    bool has_torch =
        (s.game.flags & ((uint64_t)1u << DR_FLAG_TORCH)) != 0;
    uint8_t loc = dr_world_location(&s_world);
    // 搜索行定制(M7 回补):沼泽=护符换谈话(无护符/已谈过灰显),
    // 残村=一次性拿补给(已拿灰显);灰显仍可按下,逻辑层回报原因
    const char *sv = "再掷一次";
    bool slock = false;
    if (loc == DR_WT_SWAMP) {
        if (s.game.flags & ((uint64_t)1u << DR_FLAG_SWAMP_DONE)) {
            sv = "已谈过"; slock = true;
        } else if (s.game.res[DR_RES_CHARM] < 1) {
            sv = "没有护符"; slock = true;
        } else {
            sv = "1 护符换谈话";
        }
    } else if (loc == DR_WT_RUINV) {
        if (s.game.flags & ((uint64_t)1u << DR_FLAG_RUIN_TAKEN)) {
            sv = "已拿空"; slock = true;
        } else {
            sv = "拿取补给";
        }
    }
    set_row(0, 150, "搜索", sv, s.focus == 0, slock);
    set_row(1, 174, "离开地点", "回荒野", s.focus == 1, false);
    for (int i = 2; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 200);
    lv_label_set_text(s_hint,
        loc == DR_WT_CAVE
            ? (has_torch ? "洞穴幽深;火把只够一次深入" : "需要火把(工坊:1木1布)")
        : loc == DR_WT_HOUSE ? "老屋:也许有药,也许有埋伏"
        : loc == DR_WT_TOWN  ? "废镇:学校与医院,或街头伏击"
        : loc == DR_WT_CITY  ? "废墟城市:空楼,士兵,或值钱的物资"
        : loc == DR_WT_BOREHOLE ? "钻孔:深处必见异星合金"
        : loc == DR_WT_BFIELD   ? "战场:散落的军械,也许有合金"
        : loc == DR_WT_SWAMP
            ? ((s.game.flags & ((uint64_t)1u << DR_FLAG_SWAMP_DONE))
                   ? "沼泽重归寂静"
                   : "小屋里住着老者:一枚护符换一次谈话")
        : loc == DR_WT_RUINV
            ? ((s.game.flags & ((uint64_t)1u << DR_FLAG_RUIN_TAKEN))
                   ? "残村什么都不剩了"
                   : "被毁的村庄:小屋里仍有补给")
                              : "");
}

static void render_combat(void) {
    render_topbar_c("战斗", COL_ENEMY);
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
    // 名行语义色(T4):敌红/我蓝
    lv_obj_set_style_text_color(s_cells[0], lv_color_hex(COL_ENEMY), 0);
    lv_obj_set_style_text_color(s_cells[1], lv_color_hex(COL_ME), 0);
    uint16_t ehp = dr_world_fight_hp(&s_world), ehp_max = dr_world_fight_hp_max(&s_world);
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_flag(s_hp[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hp[i], 0, 46 + i * 32);
        int w = (i == 0)
            ? (ehp_max ? (int)(198u * ehp / ehp_max) : 0)
            : (int)(198u * s.game.hero_hp / (s.game.hero_hp_max ? s.game.hero_hp_max : 1));
        lv_obj_set_size(s_hpfill[i], w, 8);
        // 血量语义色(T4):敌恒红;我 >40% 蓝 / ≤40% 琥珀 / ≤15% 红
        if (i == 0) {
            lv_obj_set_style_bg_color(s_hpfill[i],
                                      lv_color_hex(COL_ENEMY), 0);
        } else {
            int pct = s.game.hero_hp_max
                ? (int)((uint32_t)s.game.hero_hp * 100u / s.game.hero_hp_max)
                : 0;
            lv_obj_set_style_bg_color(s_hpfill[i],
                lv_color_hex(pct <= 15 ? COL_ENEMY
                                       : pct <= 40 ? COL_AMBER : COL_ME), 0);
        }
    }
    // 中段战报:最近 3 条战斗日志(血条与动作行之间旧版整段留白 80px,I18)
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = s_loglines[i];
        const char *e = log_entry_newest_first(i);
        if (!e) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_pos(l, 0, 100 + i * 15);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(l, e);
        lv_obj_set_style_text_color(l, log_fade(i, 3, log_base(i)), 0);
    }
    for (int i = 3; i < LOG_VIS; i++)
        lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);
    // 行序:攻击/[手雷]/[流星锤]/吃干肉/用药/逃跑——消耗品行仅携带时出现
    // (复用列表行,不增常驻对象;动作行与列表行混排同款金箭头焦点)
    char a[24];
    int rows = 0;
    snprintf(a, sizeof(a), "攻击(%s)", weapon_name(s.game.weapon_lv));
    set_act(&s_acts[0], a, s.focus == rows, 0);
    lv_obj_set_pos(s_acts[0].row, 0, 150 + rows * 20);
    lv_obj_clear_flag(s_acts[0].row, LV_OBJ_FLAG_HIDDEN);
    rows++;
    char v[12];
    if (dr_world_carry_grenades(&s_world) > 0) {
        snprintf(v, sizeof(v), "余%u", dr_world_carry_grenades(&s_world));
        set_row(0, 150 + rows * 20, "手雷", v, s.focus == rows, false);
        rows++;
    }
    if (dr_world_carry_bolas(&s_world) > 0) {
        snprintf(v, sizeof(v), "余%u", dr_world_carry_bolas(&s_world));
        set_row(1, 150 + rows * 20, "流星锤", v, s.focus == rows, false);
        rows++;
    }
    snprintf(a, sizeof(a), "吃干肉(余%u)", s.game.food);
    set_act(&s_acts[1], a, s.focus == rows, 0);
    lv_obj_set_pos(s_acts[1].row, 0, 150 + rows * 20);
    lv_obj_clear_flag(s_acts[1].row, LV_OBJ_FLAG_HIDDEN);
    rows++;
    snprintf(a, sizeof(a), "用药(余%u)", dr_world_carry_medicine(&s_world));
    set_act(&s_acts[2], a, s.focus == rows, 0);
    lv_obj_set_pos(s_acts[2].row, 0, 150 + rows * 20);
    lv_obj_clear_flag(s_acts[2].row, LV_OBJ_FLAG_HIDDEN);
    rows++;
    set_act(&s_acts[3], "逃跑", s.focus == rows, 0);
    lv_obj_set_pos(s_acts[3].row, 0, 150 + rows * 20);
    lv_obj_clear_flag(s_acts[3].row, LV_OBJ_FLAG_HIDDEN);
    rows++;
    if (rows <= 4) {   // 有消耗品行时提示行让位(行底已贴近内容区下缘)
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_hint, 0, 262);
        lv_label_set_text(s_hint, "命中80%;敌按攻击间隔反击");
    }
}

static void render_trade(void) {
    render_topbar_c("贸易", ACCENT_TRADE);
    render_rule(42, 2, ACCENT_TRADE);
    // 支付货币摘要:毛/鳞/牙(原版 TradeGoods 以此三者支付,只买不卖)
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_res(0, "木", s.game.res[DR_RES_WOOD]);
    cell_res(1, "毛", s.game.res[DR_RES_FUR]);
    cell_res(2, "鳞", s.game.res[DR_RES_SCALES]);
    cell_res(3, "牙", s.game.res[DR_RES_TEETH]);
    bool post = s.game.building_lv[DR_BLD_TRADE_POST] > 0;
    bool compass = (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) != 0;
    char v[28];
    if (post) {
        // 10 档货(M5 +元件/合金):行距 19,返回 236,提示 258
        snprintf(v, sizeof(v), "%u毛", 150u);  set_row(0, 46, "买 鳞", v, s.focus == 0, !trade_affordable(0));
        snprintf(v, sizeof(v), "%u毛", 300u);  set_row(1, 65, "买 牙", v, s.focus == 1, !trade_affordable(1));
        snprintf(v, sizeof(v), "%u毛%u鳞", 150u, 50u); set_row(2, 84, "买 铁", v, s.focus == 2, !trade_affordable(2));
        snprintf(v, sizeof(v), "%u毛%u牙", 200u, 50u); set_row(3, 103, "买 煤", v, s.focus == 3, !trade_affordable(3));
        snprintf(v, sizeof(v), "%u毛%u鳞%u牙", 300u, 50u, 50u); set_row(4, 122, "买 钢", v, s.focus == 4, !trade_affordable(4));
        snprintf(v, sizeof(v), "%u鳞", 10u);   set_row(5, 141, "买 子弹", v, s.focus == 5, !trade_affordable(5));
        snprintf(v, sizeof(v), "%u鳞%u牙", 50u, 30u); set_row(6, 160, "买 药", v, s.focus == 6, !trade_affordable(6));
        if (compass) snprintf(v, sizeof(v), "已购");
        else         snprintf(v, sizeof(v), "%u毛%u鳞%u牙", 400u, 20u, 10u);
        set_row(7, 179, "买 罗盘", v, s.focus == 7, !trade_affordable(7));
        snprintf(v, sizeof(v), "%u鳞%u牙", 10u, 10u); set_row(8, 198, "买 元件", v, s.focus == 8, !trade_affordable(8));
        snprintf(v, sizeof(v), "1500毛750鳞300牙");
        set_row(9, 217, "买 合金", v, s.focus == 9, !trade_affordable(9));
        set_row(10, 236, "返回", "", s.focus == 10, false);
    } else {
        for (int i = 0; i < 10; i++) set_row(i, 46 + i * 19, "—", "需贸易站", false, true);
        set_row(10, 236, "返回", "", s.focus == 10, false);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 258);
    lv_label_set_text(s_hint, "游牧商人:以毛皮/鳞/牙易货");
}

static void render_settings(void) {
    render_topbar_c("设置", COL_DIM);
    render_tabs(4, forest_open(), nav_tab_enabled(2), -1);
    // 9 行,步 25(52 起,尾行 252+22=274 收进内容区 278)
    set_row(0, 52, "操作说明", "键位:三键", s.focus == 0, false);
    set_row(1, 77, "重开本局", "需确认", s.focus == 1, false);
    // 危险项分隔线+红字(T5);焦点反色时墨字优先
    lv_obj_clear_flag(s_sep, LV_OBJ_FLAG_HIDDEN);
    if (s.focus != 1)
        lv_obj_set_style_text_color(s_list[1].t,
                                    lv_color_hex(COL_ENEMY), 0);
    char about[28];   // 关于行:版本 + 周目(M5 周目继承;缓冲按 %u 满宽留量)
    snprintf(about, sizeof(about), "v0.7·第%u周目",
             (unsigned)(s.game.prestige_count + 1u));
    set_row(2, 102, "关于", about, s.focus == 2, false);
    char seed[12];
    snprintf(seed, sizeof(seed), "%u", (unsigned)s.game.map_seed);
    set_row(3, 127, "世界种子", seed, s.focus == 3, true);   // 只读:存档同图复现
#if DR_FEATURE_SAVECODE
    set_row(4, 152, "存档", "自动·1分钟", false, true);    // 只读:落盘节奏说明
    set_row(5, 177, "存档码", "拍照保档", s.focus == 5, false);
    set_row(6, 202, "熄屏", k_scr_off_lbl[scr_off_level()],
            s.focus == 6, false);                          // 长按下键随时手动熄屏
    set_row(7, 227, "睡眠", k_sleep_lbl[sleep_mode()],
            s.focus == 7, false);                          // L2/L3 档位(§8.1)
    set_row(8, 252, "返回", "", s.focus == 8, false);
#else
    // 存档码隐藏(DR_FEATURE_SAVECODE=0):导入逐字符输 200+ 字符体验未到位;
    // 后续行上移补位
    set_row(4, 152, "存档", "自动·1分钟", false, true);    // 只读:落盘节奏说明
    set_row(5, 177, "熄屏", k_scr_off_lbl[scr_off_level()],
            s.focus == 5, false);                          // 长按下键随时手动熄屏
    set_row(6, 202, "睡眠", k_sleep_lbl[sleep_mode()],
            s.focus == 6, false);                          // L2/L3 档位(§8.1)
    set_row(7, 227, "返回", "", s.focus == 7, false);
#endif
}

// ---- 存档码导出页(拍板:6 字/页,上/下翻页,确定=下一页,尾页确定=返回,
// 长按确定=回第 1 页,顶部"第 n/N 页",页脚"拍照后按确定翻页") ----
static void render_export(void) {
    render_topbar_c("存档码", COL_DIM);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "第%d/%d页", s.code_page + 1, s.code_pages);
    lv_label_set_text(s_code_hdr, hdr);
    lv_obj_clear_flag(s_code_hdr, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < CODE_COLS; i++) {
        int idx = s.code_page * CODE_COLS + i;
        char c[2] = { 0, 0 };
        c[0] = (idx < s.code_len) ? s.code_buf[idx] : ' ';
        lv_label_set_text(s_code_char[i], c);
        lv_obj_clear_flag(s_code_char[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 256);
    lv_label_set_text(s_hint, "拍照后按确定翻页");
}

// ---- 存档码导入页(拍板:逐字符输入;已输字符 6 字/行回看 3 行;
// 输满自动校验,失败"确定=清空重来/长按=退出";双击下键随时退出) ----
static void render_import(void) {
    render_topbar_c("导入存档码", COL_DIM);
    int cur_row = s.imp_pos / CODE_COLS;
    int start = (cur_row >= 3) ? cur_row - 2 : 0;
    for (int li = 0; li < 3; li++) {
        int row = start + li;
        if (row * CODE_COLS > s.imp_pos) {   // 该行无内容(含空尾行)
            lv_obj_add_flag(s_imp_lines[li], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char line[CODE_COLS + 1];
        for (int c = 0; c < CODE_COLS; c++) {
            int idx = row * CODE_COLS + c;
            if (idx < s.imp_pos) line[c] = s.imp_buf[idx];
            else if (idx == s.imp_pos && !s.imp_failed)
                line[c] = DR_CODE_B32[s.imp_draft];   // 光标位实时预览草稿
            else
                line[c] = '_';
        }
        line[CODE_COLS] = '\0';
        lv_label_set_text(s_imp_lines[li], line);
        lv_obj_set_style_text_color(s_imp_lines[li],
            lv_color_hex(row == cur_row && !s.imp_failed ? COL_GOLD : COL_DIM), 0);
        lv_obj_clear_flag(s_imp_lines[li], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 256);
    if (s.imp_failed)
        lv_label_set_text(s_hint, "校验失败 确定=重来 长按=退出");
    else if (s.imp_total > 0) {
        char h[64];   // int 全范围最坏 58B(-Werror=format-truncation 实测)
        snprintf(h, sizeof(h), "上/下换字 确定=下一位 第%d/%d页",
                 cur_row + 1, (s.imp_total + CODE_COLS - 1) / CODE_COLS);
        lv_label_set_text(s_hint, h);
    } else
        lv_label_set_text(s_hint, "上/下换字 确定=下一位 长按=退格");
}

// 事件选项标签:文案 + 代价后缀(如"给毛·50毛");数字统一由后缀显示
static const char *k_res_str[DR_RES_KIND_COUNT] = {
    [DR_RES_WOOD] = "木", [DR_RES_FUR] = "毛", [DR_RES_MEAT] = "肉",
    [DR_RES_BAIT] = "饵", [DR_RES_WATER] = "水", [DR_RES_FOOD] = "干",
    [DR_RES_IRON] = "铁", [DR_RES_COAL] = "煤", [DR_RES_STEEL] = "钢",
    [DR_RES_SULPHUR] = "硫", [DR_RES_BULLETS] = "弹", [DR_RES_ALIEN] = "金",
    [DR_RES_CHARM] = "符", [DR_RES_LEATHER] = "革", [DR_RES_SCALES] = "鳞",
    [DR_RES_TEETH] = "牙", [DR_RES_CLOTH] = "布", [DR_RES_MEDICINE] = "药",
    [DR_RES_ENERGY_CELL] = "能", [DR_RES_GRENADE] = "雷",
    [DR_RES_BOLAS] = "锤",
};

// 旗标代价的显示名(现有:火把;新旗标代价须在此登记)
static const char *flag_cost_name(uint8_t flag) {
    return (flag == DR_FLAG_TORCH) ? "火把" : "?";
}

static const char *ev_choice_label(const dr_event_t *e, int i) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s", dr_text(e->choices[i].text_id));
    size_t n = strlen(buf);
    for (int k = 0; k < 3 && n < sizeof(buf) - 12; k++) {
        uint8_t r = e->choices[i].cost_res[k];
        if (r == DR_COST_NONE) break;
        int w;
        if (r == DR_COST_FLAG) {
            // 旗标代价:显示"火把1"(要求已持有,选中即消耗)
            w = snprintf(buf + n, sizeof(buf) - n, "%s%s1",
                         k == 0 ? "·" : " ",
                         flag_cost_name((uint8_t)e->choices[i].cost_amt[k]));
        } else {
            w = snprintf(buf + n, sizeof(buf) - n, "%s%u%s",
                         k == 0 ? "·" : " ",
                         e->choices[i].cost_amt[k], k_res_str[r]);
        }
        if (w > 0) n += (size_t)w;
    }
    return buf;
}

static void render_event(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    if (count == 0) return;               // 空表守卫:不越界读 ev[0](I11)
    if (s.ev_idx >= count) s.ev_idx = 0;
    const dr_event_t *e = &ev[s.ev_idx];
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dpanel, 22, 42 + s.dpanel_drop * 4);   // 水平居中+上滑(T7a)
    lv_obj_set_size(s_dpanel, 196, 236);
    lv_obj_add_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dbody, 0, 0);      // 面板已有 pad10,子坐标不再叠加偏移
    // 选项底部锚定(mockup ④):最后一行固定 y=164;正文占满选项以上空间
    // (≤3 选项时正文区 80→112px=7 行,给 M6 长文案留量,I12)
    int bh = 164 - (e->choice_count - 1) * 24 - 8;
    if (bh > 112) bh = 112;
    lv_obj_set_size(s_dbody, 176, bh);
    set_dbody(dr_text(e->text_id));
    for (int i = 0; i < e->choice_count && i < 5; i++) {
        actrow_t *a = &s_dacts[i];
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(a->row, 0, 164 - (e->choice_count - 1 - i) * 24);
        set_act(a, ev_choice_label(e, i), s.focus == i, 0);
        // 选项常态描边(T5);焦点反色行由 set_act 去边
        lv_obj_set_style_border_width(a->row, s.focus == i ? 0 : 1, 0);
        lv_obj_set_style_border_color(a->row, lv_color_hex(COL_BORDER), 0);
        bool ok = dr_event_choice_affordable(e, (uint8_t)i, &s.game);
        // 斥候"买地图":迷雾揭完则无可买(原版 available: !World.seenAll)
        if (e->text_id == DR_T_EV_SCOUT &&
            e->choices[i].text_id == DR_T_EV_SCOUT_MAP &&
            !dr_world_map_has_unseen(&s_world))
            ok = false;
        if (!ok) {   // 不可付:永不反色——灰字,焦点=金描边/常态=灰描边(T1)
            lv_obj_set_style_bg_opa(a->row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(a->row, 1, 0);
            lv_obj_set_style_border_color(a->row,
                lv_color_hex(s.focus == i ? COL_GOLD : COL_BORDER), 0);
            lv_obj_set_style_text_color(a->lbl, lv_color_hex(COL_DIM), 0);
        }
    }
    for (int i = e->choice_count; i < 5; i++)
        lv_obj_add_flag(s_dacts[i].row, LV_OBJ_FLAG_HIDDEN);
}

static void render_confirm(void) {
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dpanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dpanel, 22, 60 + s.dpanel_drop * 4);   // 水平居中,垂直 60+上滑(T7a)
    lv_obj_set_size(s_dpanel, 196, 200);
    lv_obj_clear_flag(s_dtitle, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_dtitle,
                      s.confirm_from == 2 ? "离开" : "确认操作");
    lv_obj_clear_flag(s_dbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dbody, 0, 42);
    lv_obj_set_size(s_dbody, 176, 40);
    set_dbody(s.confirm_from == 2
                  ? "要离开了。这一去,不会再回来。"
                  : "确定重开本局?当前进度将丢失。");
    for (int i = 0; i < 2; i++) {
        actrow_t *a = &s_dacts[i];
        lv_obj_clear_flag(a->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(a->row, 0, 130 + i * 24);
        set_act(a,
                s.confirm_from == 2 ? (i == 0 ? "升空" : "再待会儿")
                                    : (i == 0 ? "确认执行" : "取消"),
                s.focus == i, 0);
        // 选项常态描边(T5);焦点反色行由 set_act 去边
        lv_obj_set_style_border_width(a->row, s.focus == i ? 0 : 1, 0);
        if (s.focus != i)
            lv_obj_set_style_border_color(a->row,
                                          lv_color_hex(COL_BORDER), 0);
    }
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
    render_topbar_c("制造", ACCENT_CRAFT);
    render_rule(44, 2, ACCENT_CRAFT);
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_res(0, "木", s.game.res[DR_RES_WOOD]);
    cell_res(1, "革", s.game.res[DR_RES_LEATHER]);
    cell_res(2, "鳞", s.game.res[DR_RES_SCALES]);
    cell_res(3, "铁", s.game.res[DR_RES_IRON]);
    for (int i = 0; i < LOG_VIS; i++) lv_obj_add_flag(s_loglines[i], LV_OBJ_FLAG_HIDDEN);

    static const struct { uint8_t res; const char *label; } comps[] = {
        { DR_RES_WOOD, "木" },   { DR_RES_TEETH, "牙" },
        { DR_RES_LEATHER, "革" },{ DR_RES_IRON, "铁" },
        { DR_RES_STEEL, "钢" },  { DR_RES_SULPHUR, "硫" },
        { DR_RES_CLOTH, "布" },
    };
    // 先收齐全部可见项,再做 8 行窗口(14 项全解锁时旧版硬截 9 行,
    // 木桶/水箱/背囊/篷车/车队永远选不到,I8)
    uint8_t vis[DR_CRAFT_KIND_COUNT];
    int total = 0;
    for (int c = 0; c < DR_CRAFT_KIND_COUNT; c++) {
        if (dr_rules_craft_visible(&s.game, (uint8_t)c))
            vis[total++] = (uint8_t)c;
    }
    s.craft_total = (uint8_t)total;
    int rows = (total < 8) ? total : 8;   // 窗口 8 行:返回行 236 + 提示行 262 收进 278
    if (s.craft_scroll + rows > total)
        s.craft_scroll = (uint8_t)((total > rows) ? total - rows : 0);
    for (int row = 0; row < rows; row++) {
        uint8_t c = vis[s.craft_scroll + row];
        char t[20], v[64];
        snprintf(t, sizeof(t), "%s", craft_name(c));
        bool owned = dr_rules_craft_owned(&s.game, c);
        bool can = dr_rules_craft_ready(&s.game, c);
        if (owned && c != DR_CRAFT_TORCH) {
            snprintf(v, sizeof(v), "已有");
        } else if (!can) {
            // 材料不足:直接展示缺口(与建造页同款约定),不必回房翻日志(I29)
            int n = snprintf(v, sizeof(v), "缺");
            for (int k = 0; k < 7 && n > 0 && n < (int)sizeof(v); k++) {
                uint32_t need = dr_rules_craft_need(&s.game, c,
                                                    comps[k].res);
                if (!need) continue;
                uint32_t have = s.game.res[comps[k].res];
                if (need <= have) continue;
                n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                              (unsigned long)(need - have), comps[k].label);
            }
            if (n <= 1) snprintf(v, sizeof(v), "需工坊");   // 无材料缺口=工坊门槛
        } else {
            int n = 0;
            for (int k = 0; k < 7; k++) {
                uint32_t need = dr_rules_craft_need(&s.game, c,
                                                    comps[k].res);
                if (!need) continue;
                if (n == 0) n = snprintf(v, sizeof(v), "%lu%s",
                                         (unsigned long)need, comps[k].label);
                else n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                   (unsigned long)need, comps[k].label);
            }
            if (n == 0) snprintf(v, sizeof(v), "免费");
        }
        set_row(row, 52 + row * 23, t, v, s.focus == row, !can);
        s.craft_row_map[row] = c;
    }
    s.craft_row_count = (uint8_t)rows;
    for (int i = rows; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    set_row(rows, 52 + rows * 23, "返回", "", s.focus == rows, false);
    if (s.craft_total > rows) {
        // 滚动指示条:窗口贴右缘,滑块标示 8 行窗在 total 项中的位置
        int track_h = rows * 23 - 1;
        int thumb_h = track_h * rows / s.craft_total;
        lv_obj_set_pos(s_strack, 195, 52);
        lv_obj_set_size(s_strack, 2, track_h);
        lv_obj_clear_flag(s_strack, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_sthumb, 195,
                       52 + (int)s.craft_scroll * (track_h - thumb_h) /
                             (s.craft_total - rows));
        lv_obj_set_size(s_sthumb, 2, thumb_h);
        lv_obj_clear_flag(s_sthumb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 262);
    lv_label_set_text(s_hint, "武器/护甲自动装备取最优;水具/背具即刻生效");
}

// ---- 星舰页(原版 An Old Starship 页签;M5) ----
// 行序:加固船壳/升级引擎/[制造…]/升空/返回(制造行在执行者了结后出现)
static int ship_rows(void) { return dr_fab_unlocked(&s.game) ? 5 : 4; }

static void render_ship(void) {
    render_topbar_c("星舰", ACCENT_SHIP);
    render_tabs(3, forest_open(), nav_tab_enabled(2), -1);
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 48);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "壳 %u", s.game.ship_hull);
    lv_label_set_text_fmt(s_cells[1], "推 %u", s.game.ship_thrusters);
    cell_res(2, "金", s.game.res[DR_RES_ALIEN]);
    cell_res(3, "能", s.game.res[DR_RES_ENERGY_CELL]);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool no_alloy = s.game.res[DR_RES_ALIEN] < DR_SHIP_ALLOY_PER_HULL;
    char v[24];
    int row = 0;
    snprintf(v, sizeof(v), "船壳%u→%u", s.game.ship_hull, s.game.ship_hull + 1);
    set_row(row, 96 + row * 24, "加固船壳", v, s.focus == row, no_alloy); row++;
    snprintf(v, sizeof(v), "推进%u→%u", s.game.ship_thrusters,
             s.game.ship_thrusters + 1);
    set_row(row, 96 + row * 24, "升级引擎", v, s.focus == row, no_alloy); row++;
    if (dr_fab_unlocked(&s.game)) {
        set_row(row, 96 + row * 24, "制造…", "制造台", s.focus == row, false);
        row++;
    }
    if (!dr_ship_can_liftoff(&s.game))
        snprintf(v, sizeof(v), "需船壳");
    else if ((int32_t)(s.liftoff_cd_ms - now_ms) > 0)
        snprintf(v, sizeof(v), "%lds",
                 (long)((s.liftoff_cd_ms - now_ms + 999) / 1000));
    else
        snprintf(v, sizeof(v), "就绪");
    // 与出发远征同款:不可升空/冷却中置灰但可聚焦(确定给提示,不做死键)
    set_row(row, 96 + row * 24, "升空", v, s.focus == row,
            !dr_ship_can_liftoff(&s.game) ||
                (int32_t)(s.liftoff_cd_ms - now_ms) > 0);
    row++;
    set_row(row, 96 + row * 24, "返回", "", s.focus == row, false);
    for (int i = row + 1; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 236);
    if (no_alloy) {
        lv_label_set_text(s_hint, "异星合金:钻孔必得,战场/贸易/病人");
    } else {
        char h[40];
        snprintf(h, sizeof(h), "推进%u 磨难段%u(段均船壳-1)",
                 s.game.ship_thrusters, dr_ship_drift_segments(&s.game));
        lv_label_set_text(s_hint, h);
    }
}

// ---- 制造台页(星舰页"制造…"进入;M5 fabricator) ----
static const char *fab_name(uint8_t f) {
    static const char *n[DR_FAB_KIND_COUNT] = {
        "刺刀", "激光步枪", "手雷", "流星锤",
    };
    return n[f];
}

static void render_fab(void) {
    render_topbar_c("制造台", ACCENT_CRAFT);
    render_rule(44, 2, ACCENT_CRAFT);   // T7e:无页签列表页补规则线(星舰有页签不加)
    for (int i = 0; i < 4; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 4; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    cell_res(0, "木", s.game.res[DR_RES_WOOD]);
    cell_res(1, "能", s.game.res[DR_RES_ENERGY_CELL]);
    cell_res(2, "鳞", s.game.res[DR_RES_SCALES]);
    cell_res(3, "牙", s.game.res[DR_RES_TEETH]);
    static const struct { uint8_t res; const char *label; } comps[] = {
        { DR_RES_WOOD, "木" }, { DR_RES_IRON, "铁" },
        { DR_RES_ENERGY_CELL, "能" }, { DR_RES_SCALES, "鳞" },
        { DR_RES_TEETH, "牙" },
    };
    for (int f = 0; f < DR_FAB_KIND_COUNT; f++) {
        char v[40];
        bool owned = dr_fab_owned(&s.game, (uint8_t)f);
        bool can = dr_fab_ready(&s.game, (uint8_t)f);
        if (owned) {
            snprintf(v, sizeof(v), "已有");
        } else {
            int n = 0;
            for (int k = 0; k < 5; k++) {
                uint32_t need = dr_fab_need(&s.game, (uint8_t)f, comps[k].res);
                if (!need) continue;
                if (!can && s.game.res[comps[k].res] < need) {
                    if (n == 0) n = snprintf(v, sizeof(v), "缺");
                    n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                  (unsigned long)(need - s.game.res[comps[k].res]),
                                  comps[k].label);
                } else {
                    n += snprintf(v + n, sizeof(v) - n, "%lu%s",
                                  (unsigned long)need, comps[k].label);
                }
            }
        }
        set_row(f, 52 + f * 23, fab_name((uint8_t)f), v, s.focus == f, !can);
    }
    set_row(4, 52 + 4 * 23, "返回", "", s.focus == 4, false);
    for (int i = 5; i < LIST_ROWS; i++)
        lv_obj_add_flag(s_list[i].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_hint, 0, 170);
    lv_label_set_text(s_hint, "武器取最优;雷/锤可重复造");
}

// ---- 升空叙事页(M5 拍板:替代原版 2D 太空射击) ----
#define LIFT_CRASH 200u   // lift_seg 特值:坠毁段
static const char *const k_lift_seg[] = {
    "点火。引擎的轰鸣震得舱壁发颤,尘土从船身上簌簌落下。",
    "对流层。舷窗外云层翻涌,像一锅烧开的灰汤。",
    "平流层。天空褪成一种更深的蓝,村庄小得像一粒尘。",
    "中间层。星光在白昼里显形,冷而硬。",
    "热层。船壳微微发烫,仪表的光在黑暗里跳动。",
    "逃逸层。重力终于松开了手。",
    "太空。死一样的寂静,只剩引擎的低鸣。",
    "远处有一个光点,固执地亮着。",
};
static const char k_lift_drift[] =
    "燃料不足的警告响起。船身在黑暗里漂流,撞上一片碎片。";
static const char k_lift_crash[] =
    "船壳裂开了。警报声慢下来,一切慢下来,然后是漫长的坠落。";

static uint8_t lift_ndrift(void) {
    return (s.lift_total > 8) ? (uint8_t)(s.lift_total - 8) : 0;
}

static void render_liftoff(void) {
    render_topbar("升空");
    for (int i = 0; i < 2; i++) {
        cell_base(i, i * 49, 24);
        lv_obj_clear_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 2; i < 8; i++) lv_obj_add_flag(s_cells[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_cells[0], "壳 %u", s.game.ship_hull);
    lv_label_set_text_fmt(s_cells[1], "推 %u", s.game.ship_thrusters);
    const char *txt;
    if (s.lift_seg == LIFT_CRASH) txt = k_lift_crash;
    else if (s.lift_seg < 7) txt = k_lift_seg[s.lift_seg];
    else if (s.lift_seg < 7 + lift_ndrift()) txt = k_lift_drift;
    else txt = k_lift_seg[7];   // 光点(固定尾段)
    set_story(txt);
    set_act(&s_acts[0], "继续", s.focus == 0, 0);
    lv_obj_set_pos(s_acts[0].row, 0, 236);
    lv_obj_clear_flag(s_acts[0].row, LV_OBJ_FLAG_HIDDEN);
}

// ---- 结局页(M5) ----
static const char k_ending[] =
    "飞船掠过碎片云。巨大的世界舰静静漂着,伤痕累累,再没有灯火。\n"
    "空气越来越薄,舱里越来越冷。\n"
    "你把坐标交给了星星。剩下的路,让它们领。\n"
    "——你离开了这颗星球。";

static void render_ending(void) {
    render_topbar("结局");
    set_story(k_ending);
    set_act(&s_acts[0], "继续游玩", s.focus == 0, 0);
    lv_obj_set_pos(s_acts[0].row, 0, 212);
    lv_obj_clear_flag(s_acts[0].row, LV_OBJ_FLAG_HIDDEN);
    set_act(&s_acts[1], "开新周目", s.focus == 1, 0);
    lv_obj_set_pos(s_acts[1].row, 0, 236);
    lv_obj_clear_flag(s_acts[1].row, LV_OBJ_FLAG_HIDDEN);
}

static void render_title(void) {
    static const char sub[] = "屋里冷得刺骨";
    lv_label_set_text(s_topbar, "");
    lv_label_set_text(s_batt, "");
    lv_obj_clear_flag(s_t_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_ok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_t_fire, LV_OBJ_FLAG_HIDDEN);
    // 打字机(T3):250ms/字逐显 6 字;未完时由 tick 置脏续拍,完则静止
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_title_reveal_ms == 0) s_title_reveal_ms = now;
    int k = (int)((now - s_title_reveal_ms) / 250u);
    if (k > 6) k = 6;
    char buf[24];
    if (k > 0) {
        memcpy(buf, sub, (size_t)k * 3);   // 全 CJK,3 字节/字,整字截断
        buf[k * 3] = 0;
    } else {
        buf[0] = 0;
    }
    lv_label_set_text(s_t_sub, buf);
    // T7b:点燃/渐显与打字机同拍幂等推进——k 拍时火光带亮 k/6,标题显 k*85
    lv_obj_set_style_bg_grad_opa(s_t_fire, 89 * k / 6, 0);
    int opa = (k * 85 > 255) ? 255 : k * 85;
    lv_obj_set_style_text_opa(s_t_big, (lv_opa_t)opa, 0);
    lv_obj_set_style_text_opa(s_t_rule, (lv_opa_t)opa, 0);
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
        case PG_SHIP:    render_ship(); break;
        case PG_FAB:     render_fab(); break;
        case PG_LIFTOFF: render_liftoff(); break;
        case PG_ENDING:  render_ending(); break;
        case PG_EXPORT:  render_export(); break;
        case PG_IMPORT:  render_import(); break;
    }
    // 共享焦点条定位(T7d):页面渲染完行几何后统一驱动;同页移动触发
    // 80ms y 补间(补间中不重写位置),跨页/首现直接落位
    lv_obj_t *bar_row = (s_bar_act != NULL) ? s_bar_act->row
                        : (s_bar_row >= 0) ? s_list[s_bar_row].row : NULL;
    if (bar_row != NULL && !lv_obj_has_flag(bar_row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_update_layout(bar_row);   // set_pos 后 coords 要等布局周期,先同步
        int bx = lv_obj_get_x(bar_row);
        int by = lv_obj_get_y(bar_row);
        int bw = lv_obj_get_width(bar_row);
        lv_obj_clear_flag(s_focusbar, LV_OBJ_FLAG_HIDDEN);
        if (!s_bar_moving) {
            lv_obj_set_pos(s_focusbar, bx, by);
            lv_obj_set_size(s_focusbar, bw, 18);
            if (s_bar_shown && by != s_bar_last_y) {
                s_bar_moving = true;
                lv_anim_delete(s_focusbar, bar_y_cb);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_focusbar);
                lv_anim_set_exec_cb(&a, bar_y_cb);
                lv_anim_set_values(&a, s_bar_last_y, by);
                lv_anim_set_duration(&a, 80);
                lv_anim_set_completed_cb(&a, bar_move_done_cb);
                lv_anim_start(&a);
            }
        }
        s_bar_shown = true;
        s_bar_last_y = by;
    } else {
        lv_obj_add_flag(s_focusbar, LV_OBJ_FLAG_HIDDEN);
        s_bar_shown = false;
    }
    // 就地提示:渲染末覆盖提示行(仅对当前显示提示行的页面生效;
    // 到期由 tick 清除并触发整页重画,恢复各页自己的提示文案与暗色)
    if (s.toast[0] && !lv_obj_has_flag(s_hint, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(s_hint, s.toast);
        lv_obj_set_style_text_color(s_hint, lv_color_hex(s.toast_col), 0);
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
        case PG_MAP: return s.game.in_wilderness ? 5 : 10;
        // 远征:东南西北+吃干肉 / 整备:6 携带行+武器(只读)/护甲(只读)/出发/返回
        case PG_RUIN: return 2;    // 搜索 / 离开
        case PG_COMBAT:            // 攻击/吃/药/逃 + 携带中的手雷/流星锤行
            return 4 + (dr_world_carry_grenades(&s_world) > 0 ? 1 : 0) +
                       (dr_world_carry_bolas(&s_world) > 0 ? 1 : 0);
        case PG_TRADE: return 11;   // 10 档货(M5 +元件/合金)+ 返回
#if DR_FEATURE_SAVECODE
        case PG_SETTINGS: return 9;   // 说明/重开/关于/种子/存档/存档码/熄屏/睡眠/返回
#else
        case PG_SETTINGS: return 8;   // 说明/重开/关于/种子/存档/熄屏/睡眠/返回(码隐藏)
#endif
        case PG_EVENT: {
            uint16_t c = 0;
            const dr_event_t *ev = dr_events_table(&c);
            return ev[s.ev_idx].choice_count;
        }
        case PG_CONFIRM: return 2;
        case PG_SHIP:    return ship_rows();
        case PG_FAB:     return DR_FAB_KIND_COUNT + 1;   // 4 制造项 + 返回
        case PG_LIFTOFF: return 1;                       // 继续
        case PG_ENDING:  return 2;                       // 继续游玩/开新周目
        default: return 1;
    }
}

// 建造行可用:材料足额付得起(可见性规则之上的硬门槛;不足的行不可选,
// 光标跳过——三键约定"锁定或资源不足的行自动跳过")
static bool build_affordable(uint8_t id) {
    // 足额可付须查全部造价组件(与 dr_rules_build 同口径)——只查木/毛/肉会让
    // 革/鳞不足的行(如工坊 800木100革10鳞)白字可选中,按确定才报"材料不够"
    // 且只落日志(I29)。查全后行自动灰显并在 v 列展示"缺X革Y鳞"。
    dr_bld_cost_t c = dr_building_cost(id, s.game.building_lv[id]);
    if (c.wood == 0xFFFFFFFFu) return false;
    return s.game.res[DR_RES_WOOD] >= c.wood &&
           s.game.res[DR_RES_FUR] >= c.fur &&
           s.game.res[DR_RES_MEAT] >= c.meat &&
           s.game.res[DR_RES_IRON] >= c.iron &&
           s.game.res[DR_RES_COAL] >= c.coal &&
           s.game.res[DR_RES_STEEL] >= c.steel &&
           s.game.res[DR_RES_SULPHUR] >= c.sulphur &&
           s.game.res[DR_RES_LEATHER] >= c.leather &&
           s.game.res[DR_RES_SCALES] >= c.scales;
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
        case DR_TRADE_ENERGY_CELL:
            return s.game.res[DR_RES_SCALES] >= 10u &&
                   s.game.res[DR_RES_TEETH] >= 10u;
        case DR_TRADE_ALIEN:return s.game.res[DR_RES_FUR] >= 1500u &&
                                      s.game.res[DR_RES_SCALES] >= 750u &&
                                      s.game.res[DR_RES_TEETH] >= 300u;
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
            return idx == 10 || trade_affordable((uint8_t)idx);
        case PG_MAP:
            if (!s.game.in_wilderness) {
                if (idx == 0 || idx == 9) return true;         // 干肉 / 返回
                if (idx == 1) return s.game.res[DR_RES_MEDICINE] > 0 ||
                                   dr_world_carry_medicine(&s_world) > 0;
                if (idx == 2) return s.game.res[DR_RES_BULLETS] > 0 ||
                                   dr_world_carry_bullets(&s_world) > 0;
                if (idx == 3) return s.game.res[DR_RES_ENERGY_CELL] > 0 ||
                                   dr_world_carry_cells(&s_world) > 0;
                if (idx == 4) return s.game.res[DR_RES_GRENADE] > 0 ||
                                   dr_world_carry_grenades(&s_world) > 0;
                if (idx == 5) return s.game.res[DR_RES_BOLAS] > 0 ||
                                   dr_world_carry_bolas(&s_world) > 0;
                if (idx == 6 || idx == 7) return false;        // 武器/护甲只读
                // idx 8 = 出发:需已带干肉。冷却中仍可选(渲染置灰+确定给
                // toast 反馈)——此前冷却期静默跳焦,用户"点了没反应"(真机反馈)
                return s.game.food > 0;
            }
            if (idx == 4) return s.game.food > 0;   // 吃干肉
            return idx < 4;                          // 东南西北
        case PG_CRAFT:
            return idx >= s.craft_row_count ||      // 返回
                   dr_rules_craft_ready(&s.game, s.craft_row_map[idx]);
        case PG_EVENT: {
            uint16_t c = 0;
            const dr_event_t *ev = dr_events_table(&c);
            const dr_event_choice_t *ch = &ev[s.ev_idx].choices[idx];
            // 斥候"买地图":迷雾揭完则不可选(原版 available: !World.seenAll)
            if (ev[s.ev_idx].text_id == DR_T_EV_SCOUT &&
                ch->text_id == DR_T_EV_SCOUT_MAP &&
                !dr_world_map_has_unseen(&s_world))
                return false;
            return dr_event_choice_affordable(&ev[s.ev_idx], (uint8_t)idx,
                                              &s.game);
        }
        case PG_SETTINGS:
            return idx != 3 && idx != 4;            // 世界种子/存档为只读行
        case PG_SHIP: {
            bool fab = dr_fab_unlocked(&s.game);
            if (idx == 0 || idx == 1)                // 加固/升级:需异星合金
                return s.game.res[DR_RES_ALIEN] >= DR_SHIP_ALLOY_PER_HULL;
            if (fab && idx == 2) return true;        // 制造…
            if (idx == ship_rows() - 1) return true; // 返回
            return true;   // 升空:不可升空也可聚焦(确定给提示,与出发同款)
        }
        case PG_FAB:
            return idx >= DR_FAB_KIND_COUNT ||
                   dr_fab_ready(&s.game, (uint8_t)idx);
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

// ---- T7a 转场:切页 veil 淡入。veil 的 opa 不被 render()/clear_scene 改写
// (只碰显隐标志),是动画安全锚点;clear_scene 在淡入进行中不藏 veil。
static void veil_opa_cb(void *v, int32_t o) {
    lv_obj_set_style_opa((lv_obj_t *)v, (lv_opa_t)o, 0);
}

static void veil_fade_done_cb(lv_anim_t *a) {
    (void)a;   // veil 全局单例,无需经 anim 取回
    s_veil_fading = false;
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
}

// ---- T7d 共享焦点条:同页 y 补间(80ms);单例静态驱动,无跨页滑动 ----
static void bar_y_cb(void *v, int32_t y) {
    lv_obj_set_pos((lv_obj_t *)v, 0, y);
}

static void bar_move_done_cb(lv_anim_t *a) {
    (void)a;
    s_bar_moving = false;
    lv_obj_set_pos(s_focusbar, 0, s_bar_last_y);
}

static void veil_fade_start(void) {
    lv_anim_delete(s_veil, veil_opa_cb);   // 连切:删旧淡入再启
    s_veil_fading = true;
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_veil, LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_veil);
    lv_anim_set_exec_cb(&a, veil_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, veil_fade_done_cb);
    lv_anim_start(&a);
}

static void page_goto(page_t p) {
    s.page = p;
    s.focus = 0;
    s.village_adj = -1;   // 离开村庄页/任意切页退出人数调节
    s.outfit_adj = -1;    // 离开整备态
    s_title_reveal_ms = 0;   // 标题打字机基准(T3):进标题页首帧重记
    s.dpanel_drop = (p == PG_EVENT || p == PG_CONFIRM) ? 2 : 0;   // T7a 上滑
    s.log_focus = false;    // T8:日志窗聚焦/选中态只在主页存续,切页即退
    s.log_browse = false;
    s.log_off = 0;
    // T7d:切页收场——金条隐藏、补间删除、归属重置(新页由 render 尾重新注册)
    lv_anim_delete(s_focusbar, bar_y_cb);
    s_bar_moving = false;
    lv_obj_add_flag(s_focusbar, LV_OBJ_FLAG_HIDDEN);
    s_bar_act = NULL;
    s_bar_row = -1;
    s_bar_shown = false;
    // T7a:非模态页切页黑场淡入;模态页 veil 作常驻背板,不淡出
    if (p != PG_EVENT && p != PG_CONFIRM) veil_fade_start();
    // 回到小屋页 = 拜访房间(原版 onArrival):陌生人从沉睡中醒来帮忙
    if (p == PG_HOME && dr_rules_builder_visit(&s.game)) {
        log_push("她站在火边:可以帮忙了");
        s.save_pending = true;
    }
    if (p == PG_SHIP) {
        // 首访通知(原版 Ship.onArrival seenShip)
        if (!(s.game.flags & ((uint64_t)1u << DR_FLAG_SHIP_SEEN))) {
            s.game.flags |= (uint64_t)1u << DR_FLAG_SHIP_SEEN;
            log_push("碎片云之上,流浪者舰队仍在盘旋");
            log_push("你在这颗星球上待得太久了");
            s.save_pending = true;
        }
        // 执行者未了结:进入即触发(原版星舰守卫;事件触发必须走 page_goto,
        // 这里在同一调用内完成切页,等价于递归调 page_goto(PG_EVENT))
        if (s.game.exec_state == 0) {
            uint16_t idx = dr_events_find(DR_T_EV_EXEC);
            if (idx != 0xFFFFu) {
                s.ev_idx = idx;
                s.ev_from = (uint8_t)PG_SHIP;
                s.page = PG_EVENT;
                s.focus = 0;
            }
        }
    }
    // I35:荒野行进方向记忆——场景(战斗/地点)结束回地图页时,焦点落回
    // 上次走的方向(原一律重置 0=东:往西走被场景打断后按确定反向东走)
    if (p == PG_MAP && s.game.in_wilderness && s.last_dir >= 0)
        s.focus = s.last_dir;
    if (p == PG_EXPORT) {   // 进页生成整码一次(码在挂机中会过时,按页重进即刷新)
        s.code_len = (int)dr_savecode_export(&s.game, s.code_buf,
                                             sizeof(s.code_buf));
        if (s.code_len <= 0) s.code_len = 0;
        s.code_pages = (s.code_len + 5) / 6;
        if (s.code_pages < 1) s.code_pages = 1;
        s.code_page = 0;
        // 全码进串口日志:拍照抄码之外的第二条抄码路径(模拟器 round-trip
        // 回归也靠它)
        if (s.code_len > 0)
            ESP_LOGI(TAG, "存档码(%d字/ %d页):%s", s.code_len, s.code_pages,
                     s.code_buf);
    }
    if (p == PG_IMPORT) {   // 逐字符状态机复位
        s.imp_pos = 0;
        s.imp_total = 0;
        s.imp_draft = 0;
        s.imp_failed = false;
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
            toast("还需要等等");
        } else {
            dr_rules_gather(&s.game, dr_port_now_ts());
            s.rules_rt.gather_ready_ms = now_ms + DR_GATHER_COOLDOWN_S * 1000u;
            const char *g = s.game.building_lv[DR_BLD_CART] > 0
                          ? "收集到 50 木" : "收集到 10 木";
            log_push(g);
            toast(g);
            s.save_pending = true;
        }
        return;
    }
    if (idx == 1) {                           // 查看陷阱(90s)
        dr_trap_yield_t y;
        if (!dr_rules_trap_check(&s.rules_rt, &s.game, now_ms, &y)) {
            log_push("刚查过陷阱,再等等");
            toast("刚查过,再等等");
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
            toast(line);
            s.save_pending = true;
        }
        return;
    }
}

// 升空开始:冷却 120s(原版 LIFTOFF_COOLDOWN,按下即计);段数=8 固定+磨难段
static void liftoff_begin(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s.liftoff_cd_ms = now_ms + DR_SHIP_LIFTOFF_COOLDOWN_S * 1000u;
    s.lift_total = (uint8_t)(8 + dr_ship_drift_segments(&s.game));
    s.lift_seg = 0;
    log_push("升空!");
    page_goto(PG_LIFTOFF);
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
                toast(line);
                s.save_pending = true;
            } else if (s.game.temp_lv <= DR_TEMP_COLD) {
                log_push("屋里太冷,先点火再建造");
                toast("屋里太冷,先点火");
            } else {
                log_push("材料不够");
                toast("材料不够");
            }
            break;
        case PG_VILLAGE:
            if (idx == 8) { page_goto(PG_HOME); break; }   // 返回(行8,滚动改版后移位)
            if (idx == 0 || idx == 1) { village_action(idx); break; }
            if (idx == 2) { log_push("采集者自动拾柴(+1木)"); break; }
            {   // 行3..6:进入人数调节
                int job = village_row_job(idx);
                if (job < 0 || !dr_rules_job_unlocked(&s.game, (uint8_t)job)) {
                    const char *m = job == DR_JOB_TANNER ? "需先建制革坊" :
                                    job == DR_JOB_CHARCUTIER ? "需先建熏肉房" :
                                                               "需先建猎人小屋";
                    log_push(m);
                    toast(m);
                } else {
                    s.village_adj = (int8_t)job;   // 进入人数调节
                    s.dirty = true;
                }
            }
            break;
        case PG_MAP: {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (!s.game.in_wilderness) {
                if (idx <= 5) {                     // 进入携带调配
                    s.outfit_adj = (int8_t)idx;
                    s.dirty = true;
                    break;
                }
                if (idx == 8) {                     // 出发远征(需已带干肉)
                    if ((int32_t)(now_ms - s.embark_cd_ms) < 0) {
                        log_push("歇一歇再出发");
                        toast("歇一歇再出发");
                    } else if (dr_world_embark(&s_world, &s.game)) {
                        log_push("踏上尘土路");
                        // 焦点回"东"行:不重置会停在出发行位置(远征态=北行),
                        // 玩家第一按就反向(I13)
                        s.focus = 0;
                        s.last_dir = -1;   // 新一趟远征:方向记忆清零(I35)
                        s.save_pending = true;
                    } else {
                        log_push("先整备干肉(库→带)");
                        toast_c("先整备干肉", COL_ENEMY);
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
                s.last_dir = (int8_t)idx;   // I35:行进方向记忆(场景回页恢复焦点)
                uint64_t flags_before = s.game.flags;
                dr_move_result_t r =
                    dr_world_move(&s_world, &s.game, dirs[idx][0], dirs[idx][1]);
                switch (r) {
                    case DR_MOVE_FIGHT: {
                        char line[48];
                        snprintf(line, sizeof(line), "遭遇了%s!",
                                 dr_enemy_name(dr_world_fight_enemy(&s_world,
                                                                    &s.game)));
                        log_push_o(line, LOG_FOE);
                        page_goto(PG_COMBAT);
                        break;
                    }
                    case DR_MOVE_WARN_THIRST: log_push("口渴难忍"); break;
                    case DR_MOVE_WARN_HUNGER: log_push("饥饿来袭"); break;
                    case DR_MOVE_DEATH:
                        log_push_o("你倒在了荒野里,物资全失", LOG_FOE);
                        toast_c("倒下了,物资全失", COL_ENEMY);
                        s.embark_cd_ms = now_ms + 120u * 1000u;
                        s.last_dir = -1;   // 远征终结:方向记忆清零(I35)
                        s.save_pending = true;
                        break;
                    case DR_MOVE_HOME: {
                        log_push("回到了小屋");
                        s.last_dir = -1;   // 回家:方向记忆清零(I35)
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
                    case DR_MOVE_SHIP:
                        if (!ship_open()) {
                            s.game.flags |= (uint64_t)1u << DR_FLAG_SHIP_OPEN;
                            log_push("一艘坠毁的星舰躺在荒野上");
                            log_push("星舰页签已开启");
                            s.save_pending = true;
                        } else {
                            log_push("一艘坠毁的星舰躺在荒野上");
                        }
                        break;
                    case DR_MOVE_HOUSE:
                    case DR_MOVE_CAVE:
                    case DR_MOVE_TOWN:
                    case DR_MOVE_CITY:
                    case DR_MOVE_BOREHOLE:
                    case DR_MOVE_BFIELD:
                    case DR_MOVE_SWAMP:
                    case DR_MOVE_RUINV: {
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
            // 行序与渲染一致:攻击/[手雷]/[流星锤]/吃干肉/用药/逃跑
            int act = 0, rem = idx;
            if (rem > 0) {
                rem--;
                act = -1;
                if (dr_world_carry_grenades(&s_world) > 0) {
                    if (rem == 0) act = 1;
                    rem--;
                }
                if (act < 0 && dr_world_carry_bolas(&s_world) > 0) {
                    if (rem == 0) act = 2;
                    rem--;
                }
                if (act < 0) act = 3 + rem;
            }
            dr_fight_result_t r;
            if (act == 0)          r = dr_world_fight_attack(&s_world, &s.game);
            else if (act == 1)     r = dr_world_fight_grenade(&s_world, &s.game);
            else if (act == 2)     r = dr_world_fight_bolas(&s_world, &s.game);
            else if (act == 3)     r = dr_world_fight_eat(&s_world, &s.game);
            else if (act == 4)     r = dr_world_fight_medicine(&s_world, &s.game);
            else                   r = dr_world_fight_flee(&s_world, &s.game);
            const char *en = dr_enemy_name(dr_world_fight_enemy(&s_world, &s.game));
            (void)en;
            switch (r) {
                case DR_FIGHT_WIN:
                    log_push_o("击败了它!战利品入包", LOG_ME);
                    s.save_pending = true;
                    if (s.fight_return == PG_SHIP) {
                        // 执行者被击溃:制造台上线(与和解同口径解锁)
                        s.fight_return = PG_MAP;
                        s.game.exec_state = 2;
                        log_push("守卫瘫成一堆废铁");
                        log_push("舱室尽头,一台制造台亮了起来");
                        page_goto(PG_SHIP);
                    } else {
                        page_goto(PG_MAP);
                    }
                    break;
                case DR_FIGHT_LOSE:
                    log_push_o("你被它打倒了,物资全失", LOG_FOE);
                    toast_c("战败,物资全失", COL_ENEMY);
                    s.embark_cd_ms = (uint32_t)(esp_timer_get_time() / 1000) +
                                     120u * 1000u;
                    s.save_pending = true;
                    if (s.fight_return == PG_SHIP) {
                        // 执行者未了结:爬回小屋(下次进星舰页会再触发)
                        s.fight_return = PG_MAP;
                        page_goto(PG_HOME);
                    } else {
                        page_goto(PG_MAP);
                    }
                    break;
                case DR_FIGHT_FLED:
                    log_push_o("逃掉了", LOG_ME);
                    if (s.fight_return == PG_SHIP) {
                        s.fight_return = PG_MAP;
                        page_goto(PG_HOME);   // 逃离星舰,执行者仍守着舱门
                    } else {
                        page_goto(PG_MAP);
                    }
                    break;
                case DR_FIGHT_NONE:
                    if (act == 3) { log_push("没有干肉了"); toast_c("没有干肉了", COL_ENEMY); }
                    if (act == 4) { log_push("没有药了");   toast_c("没有药了", COL_ENEMY); }
                    break;
                case DR_FIGHT_STUN:       log_push_o("它被流星锤缠住了", LOG_FOE); break;
                case DR_FIGHT_MISS:       log_push_o("挥空了", LOG_ME); break;
                case DR_FIGHT_HIT:        log_push_o("命中!", LOG_ME); break;
                case DR_FIGHT_ENEMY_HIT:  log_push_o("它反击得手", LOG_FOE); break;
                case DR_FIGHT_ENEMY_MISS: log_push_o("它扑了个空", LOG_FOE); break;
                case DR_FIGHT_ENEMY_SKIP: log_push_o("它蓄势未发", LOG_FOE); break;
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
                    toast(msg);
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
                case DR_LOC_EMPTY:      log_push("这里被搜空了"); toast("被搜空了"); break;
                case DR_LOC_NEED_TORCH: log_push("需要火把(工坊:1木1布)"); toast("需要火把"); break;
                case DR_LOC_NEED_CHARM: log_push("老者要一枚护符(陷阱偶得)"); toast("需要护符"); break;
                default: break;
            }
            s.dirty = true;
            break;
        }
        case PG_TRADE: {
            if (idx == 10) { page_goto(PG_HOME); break; }
            static const char *goods[DR_TRADE_KIND_COUNT] = {
                "鳞", "牙", "铁", "煤", "钢", "子弹", "药", "罗盘",
                "能量元件", "异星合金",
            };
            if (idx == DR_TRADE_COMPASS &&
                (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS))) {
                log_push("已经有罗盘了");
                toast("已经有罗盘了");
                break;
            }
            if (dr_rules_trade_buy(&s.game, (uint8_t)idx)) {
                char line[32];
                snprintf(line, sizeof(line), "买到 %s", goods[idx]);
                log_push(line);
                toast(line);
                s.save_pending = true;
            } else {
                log_push("货款不够");
                toast("货款不够");
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
                toast(line);
                s.save_pending = true;
            } else {
                log_push("材料不够或需要工坊");
                toast("材料不够或需工坊");
            }
            break;
        }
        case PG_SHIP: {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool fab = dr_fab_unlocked(&s.game);
            if (idx == 0) {                       // 加固船壳(1 合金/点,原版)
                if (dr_ship_reinforce(&s.game)) {
                    char line[32];
                    snprintf(line, sizeof(line), "船壳加固到 %u",
                             s.game.ship_hull);
                    log_push(line); toast(line);
                    s.save_pending = true;
                } else { log_push("没有异星合金"); toast("没有异星合金"); }
                break;
            }
            if (idx == 1) {                       // 升级引擎(1 合金/个,原版)
                if (dr_ship_upgrade(&s.game)) {
                    char line[32];
                    snprintf(line, sizeof(line), "推进器增至 %u",
                             s.game.ship_thrusters);
                    log_push(line); toast(line);
                    s.save_pending = true;
                } else { log_push("没有异星合金"); toast("没有异星合金"); }
                break;
            }
            if (fab && idx == 2) { page_goto(PG_FAB); break; }
            if (idx == ship_rows() - 1) { page_goto(PG_HOME); break; }
            // 升空(需 hull>0;冷却 120s;首飞先确认,原版 seenWarning)
            if (!dr_ship_can_liftoff(&s.game)) {
                log_push("船壳太脆,先加固"); toast("先加固船壳");
                break;
            }
            if ((int32_t)(s.liftoff_cd_ms - now_ms) > 0) {
                log_push("引擎还在冷却"); toast("引擎还在冷却");
                break;
            }
            if (!(s.game.flags & ((uint64_t)1u << DR_FLAG_LIFT_WARNED))) {
                s.confirm_from = 2;
                page_goto(PG_CONFIRM);
                break;
            }
            liftoff_begin();
            break;
        }
        case PG_FAB: {
            if (idx >= DR_FAB_KIND_COUNT) { page_goto(PG_SHIP); break; }
            if (dr_fab_make(&s.game, (uint8_t)idx)) {
                char line[40];
                snprintf(line, sizeof(line), "造成了 %s",
                         fab_name((uint8_t)idx));
                log_push(line); toast(line);
                s.save_pending = true;
            } else {
                log_push("材料不够"); toast("材料不够");
            }
            break;
        }
        case PG_SETTINGS:
#if DR_FEATURE_SAVECODE
            if (idx == 8) page_goto(PG_HOME);
#else
            if (idx == 7) page_goto(PG_HOME);   // 存档码隐藏后返回行 8→7
#endif
            else if (idx == 0) log_push("上下/确定/长按返回/双击下熄屏");
            else if (idx == 1) {                       // 重开本局:需确认
                s.confirm_from = 0;
                page_goto(PG_CONFIRM);
            }
            else if (idx == 3) log_push("种子决定地图与随机序列");
            else if (idx == 4) log_push("每分钟或按键后自动落盘");
#if DR_FEATURE_SAVECODE
            else if (idx == 5) page_goto(PG_EXPORT);   // 存档码:短按=导出(长按=导入,见 LONG 分支)
            else if (idx == 6) {                       // 熄屏档位循环(L1)
#else
            else if (idx == 5) {                       // 熄屏档位循环(L1;码隐藏后前移)
#endif
                uint8_t lv = (uint8_t)((scr_off_level() + 1) % 5);
                s.game._rsv[1] = (uint8_t)((s.game._rsv[1] & ~0x07u) | lv);
                s.save_pending = true;
                log_push(lv ? "熄屏已开,双击下键手动熄屏" : "熄屏已关");
            }
            else if (idx == (DR_FEATURE_SAVECODE ? 7 : 6)) {   // 睡眠模式循环(L2/L3)
                uint8_t m = (uint8_t)((sleep_mode() + 1) % 4);
                s.game._rsv[1] = (uint8_t)((s.game._rsv[1] & 0x07u) | (m << 3));
                s.save_pending = true;
                log_push(m ? "睡眠已开:无操作自动休眠" : "睡眠已关");
            }
            else {                                     // 关于:版本 + 通关纪录
                log_push("《小黑屋》A Dark Room 重制 v0.7");
                if (s.game.prestige_count) {
                    char line[32];
                    snprintf(line, sizeof(line), "已通关 %u 次",
                             (unsigned)s.game.prestige_count);
                    log_push(line);
                }
            }
            break;
        default:
            break;
    }
}

static void handle_ok(void) {
    switch (s.page) {
        case PG_TITLE: page_goto(PG_HOME); break;
        case PG_HOME:  home_action(s.focus); break;
        case PG_EXPORT:   // 存档码两页按键在 handle_key 直接分发,不经此处
        case PG_IMPORT:   break;
        case PG_BUILD:
        case PG_VILLAGE:
        case PG_MAP:
        case PG_RUIN:
        case PG_COMBAT:
        case PG_TRADE:
        case PG_CRAFT:
        case PG_SHIP:
        case PG_FAB:
        case PG_SETTINGS: list_action(s.focus); break;
        case PG_LIFTOFF: {   // 叙事逐段推进;磨难段船壳-1,归零=坠毁
            if (s.lift_seg == LIFT_CRASH) {
                log_push("你爬出残骸,船壳彻底碎了");
                page_goto(PG_SHIP);
                break;
            }
            s.lift_seg++;
            if (s.lift_seg >= 7 && s.lift_seg < 7 + lift_ndrift()) {
                if (s.game.ship_hull > 0) s.game.ship_hull--;
                s.save_pending = true;
                if (s.game.ship_hull == 0) {
                    s.lift_seg = LIFT_CRASH;
                    log_push("燃料耗尽,船壳撞碎了");
                }
            } else if (s.lift_seg >= s.lift_total) {
                // 到达终点 = 通关(每周目结算一次 prestige)
                if (!s.game.won_this_run) {
                    dr_prestige_save(&s.game, &s.ev_rng);
                    log_push("抵达光点。通关!");
                }
                s.save_pending = true;
                page_goto(PG_ENDING);
            }
            break;
        }
        case PG_ENDING:
            if (s.focus == 0) {
                page_goto(PG_SHIP);   // 继续游玩(遗产保持待继承)
            } else {
                uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
                dr_prestige_new_game(&s.game, seed, dr_port_now_ts());
                dr_world_gen(&s_world, s.game.map_seed);   // 新周目换新图
                s_log_cnt = 0;
                char line[40];
                snprintf(line, sizeof(line), "第 %u 周目,开始了",
                         (unsigned)s.game.prestige_count + 1u);
                log_push(line);
                s.save_pending = true;
                page_goto(PG_TITLE);
            }
            break;
        case PG_EVENT: {
            uint16_t count = 0;
            const dr_event_t *ev = dr_events_table(&count);
            const dr_event_t *cur = &ev[s.ev_idx];
            uint8_t bought_ch = (uint8_t)s.focus;
            if (!dr_event_choice_affordable(cur, (uint8_t)s.focus, &s.game)) {
                log_push("身上的东西不够");   // 焦点跳过是第一道,这里兜底
                toast("身上的东西不够");
                break;
            }
            int16_t chain = dr_event_choose(ev, count, &s.game, &s.ev_sess,
                                            dr_port_now_ts(), &s.ev_rng,
                                            s.ev_idx, s.focus);
            if (chain == DR_EVENT_CHOOSE_REJECTED) break;
            s.save_pending = true;
            log_push(dr_text(cur->choices[s.focus].text_id));
            // 斥候"买地图":迷雾是会话态,引擎效果够不着——UI 桥接揭示
            // (原版 onChoose: World.applyMap)
            if (cur->text_id == DR_T_EV_SCOUT &&
                cur->choices[bought_ch].text_id == DR_T_EV_SCOUT_MAP &&
                dr_world_apply_map(&s_world, &s.game)) {
                log_push("地图上多了一片探明的区域");
            }
            // 执行者"拒绝":遭遇战由 UI 桥接启动(事件引擎够不着战斗态);
            // 敌人取士兵表(原版星舰守卫为机械系,设备适配复用遭遇敌表)
            if (cur->text_id == DR_T_EV_EXEC &&
                cur->choices[bought_ch].text_id == DR_T_EV_EXEC_REFUSE) {
                s_world.fight_enemy = DR_ENEMY_SOLDIER;
                s_world.fight_hp = 50;              // 士兵(d>20 荒地):50 HP
                s_world.fight_round = 0;
                s_world.fight_stun = 0;
                s_world.fight_from_city = false;
                s.fight_return = PG_SHIP;
                    log_push_o("守卫的独眼亮起红光,扑了过来!", LOG_FOE);
                page_goto(PG_COMBAT);
                break;
            }
            if (chain >= 0) {
                s.ev_idx = (uint16_t)chain;
                s.focus = 0;
                if (!row_enabled(0)) focus_move(1);   // 后续事件首行不可付则跳过
            } else {
                page_goto(s.ev_from == PG_VILLAGE ? PG_VILLAGE :
                          s.ev_from == PG_SHIP ? PG_SHIP : PG_HOME);
            }
            break;
        }
        case PG_CONFIRM:
            if (s.focus == 0) {
                if (s.confirm_from == 0) {
                    uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
                    dr_game_init(&s.game, seed, dr_port_now_ts());
                    dr_world_gen(&s_world, s.game.map_seed);   // 新图与新档同种
                    s_log_cnt = 0;   // 新的一局:日志清零
                    log_push("新的一局开始了");
                    s.save_pending = true;
                    page_goto(PG_SETTINGS);
                } else if (s.confirm_from == 2) {
                    // 首飞确认(原版 spaceShip.seenWarning):只问这一次
                    s.game.flags |= (uint64_t)1u << DR_FLAG_LIFT_WARNED;
                    liftoff_begin();
                } else {
                    page_goto(PG_TRADE);
                }
            } else {
                page_goto(s.confirm_from == 0 ? PG_SETTINGS :
                          s.confirm_from == 2 ? PG_SHIP : PG_TRADE);
            }
            break;
    }
    s.dirty = true;
}

// ===================================================================
// 事件触发(必须走 page_goto)
// ===================================================================
// 轮询场景:主页/村庄页各挂各的场景位(§3.3 硬化:此前只挂小屋页,
// 挂 VILLAGE 场景的事件永远不会触发)。远征/战斗/弹窗中不轮询。
static uint8_t event_scene(void) {
    switch (s.page) {
        case PG_HOME:    return DR_SCENE_HOME;
        case PG_VILLAGE: return DR_SCENE_VILLAGE;
        default:         return 0;   // 该页不轮询
    }
}

static void check_event(void) {
    uint8_t scene = event_scene();
    if (scene == 0) return;
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    uint16_t idx;
    // dr_event_poll:未触发也回写概率序列(§3.3 硬化,CHANCE 不重掷同值)
    if (dr_event_poll(ev, count, &s.game, &s.ev_sess, dr_port_now_ts(),
                      scene, &idx) == DR_EVENT_FIRED) {
        s.ev_idx = idx;
        s.ev_from = (uint8_t)s.page;   // 解决后回到来源页(村庄事件回村庄)
        page_goto(PG_EVENT);
    }
}

// ===================================================================
// 周期心跳
// ===================================================================
static void tick(lv_timer_t *t) {
    (void)t;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // 就地提示到期:清文字并还原提示行颜色,置脏触发整页重画恢复原提示
    // TODO(诊断):双堆水位心跳(系统堆+LVGL 96KB 池),卡死取证后撤
    static uint32_t s_heap_log_ms = 0;
    if (now_ms - s_heap_log_ms >= 5000u) {
        s_heap_log_ms = now_ms;
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        // I33 取证:行尾 mv=GPIO0 分压实测(定位光标自走后撤)
        ESP_LOGI(TAG, "sys=%u lvpool=%u(%u%%) frag=%u page=%d mv=%d",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)(mon.free_size), (unsigned)mon.used_pct,
                 (unsigned)mon.frag_pct, (int)s.page,
                 bsp_button_read_mv());
    }
    if (s.toast[0] && (int32_t)(now_ms - s.toast_until_ms) >= 0) {
        s.toast[0] = '\0';
        lv_obj_set_style_text_color(s_hint, lv_color_hex(COL_DIM), 0);
        s.dirty = true;
    }
    // 标题页打字机未完(T3):逐拍置脏推进揭示,完则不再重绘
    if (s.page == PG_TITLE && s_title_reveal_ms != 0 &&
        (int)((now_ms - s_title_reveal_ms) / 250u) < 6)
        s.dirty = true;
    // 弹窗上滑未归位(T7a):逐拍递减驱动重绘
    if (s.dpanel_drop > 0 && (s.page == PG_EVENT || s.page == PG_CONFIRM)) {
        s.dpanel_drop--;
        s.dirty = true;
    }

    static uint32_t last_batt_ms = 0;
    if (last_batt_ms == 0 || now_ms - last_batt_ms >= 30000u) {
        last_batt_ms = now_ms;
        int soc = bsp_battery_soc();
        // 标题页顶栏留白,电量只在游戏页刷新(§8.2"常显":含荒野/战斗等全页)
        if (soc >= 0 && s.page != PG_TITLE) {
            lv_label_set_text_fmt(s_batt, "%d%%", soc);
            // 低电红字(T4,与电量保护共存:背光/深睡管行为,红字管可见性)
            lv_obj_set_style_text_color(s_batt,
                lv_color_hex(soc <= DR_BATT_LOW_PCT ? COL_ENEMY : COL_DIM), 0);
        }
        // 电量保护(§8.2):≤20% 背光减半+提示一次;≤10% 硬保护——立即存档
        // +直接 L3 深睡(不受弹屏挂起限制,跳过 L1/L2 计时)。toast 先行,
        // 1.5s 后由 key_task 执行深睡序列(渲染任务自由跑,提示可见)。
        if (soc >= 0 && !s.deep_req) {
            if (soc <= DR_BATT_CRIT_PCT) {
                toast_c("即将沉睡", COL_ENEMY);
                s.deep_req = true;
            } else if (soc <= DR_BATT_LOW_PCT) {
                if (!s.bl_dim) {
                    s.bl_dim = true;
                    apply_backlight();
                }
                if (!s.batt_low_toast_done) {
                    toast("电量低");
                    s.batt_low_toast_done = true;
                }
            } else if (s.bl_dim) {
                s.bl_dim = false;               // 电量回升(充电中):还原背光
                apply_backlight();
            }
        }
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
            case DR_RT_EV_THIEF_START:
                log_push("库房的存货太扎眼,有人在暗中顺手牵羊");
                s.save_pending = true;
                break;
            default:
                break;
        }
    }
    if (s.page == PG_HOME || s.page == PG_VILLAGE)
        check_event();
    if (s.page == PG_HOME) {
        // 添柴冷却条按秒收短(原每 250ms 整页重绘:冷却期碎片 flush 把
        // I33 幽灵键静默窗铺满,真实按键按处理时刻撞窗被吞——真机 I34)
        int32_t s_rem = (int32_t)(s.rules_rt.stoke_ready_ms - now_ms);
        bool s_act = (s_rem > 0);
        uint8_t sec = s_act ? (uint8_t)((s_rem + 999) / 1000) : 0;
        if (sec != s.prev_stoke_sec || s_act != s.prev_stoke_active)
            s.dirty = true;
        s.prev_stoke_sec = sec;
        s.prev_stoke_active = s_act;
    }
    if (s.page == PG_VILLAGE) {
        // 采集/陷阱倒计时按秒变化才重绘(文字分辨率 1s,跳变即刷;
        // 原实现 || g_act || t_act 退化成每 250ms 整页重绘,同上吞键事故)
        int32_t g_rem = (int32_t)(s.rules_rt.gather_ready_ms - now_ms);
        int32_t t_rem = (int32_t)(s.rules_rt.trap_next_ms - now_ms);
        bool t_has = s.game.building_lv[DR_BLD_TRAP] > 0;
        bool g_act = (g_rem > 0), t_act = t_has && (t_rem > 0);
        uint8_t g_sec = g_act ? (uint8_t)((g_rem + 999) / 1000) : 0;
        uint8_t t_sec = t_act ? (uint8_t)((t_rem + 999) / 1000) : 0;
        if (g_sec != s.prev_gather_sec || t_sec != s.prev_trap_sec ||
            g_act != s.prev_gather_ready || t_act != s.prev_trap_ready)
            s.dirty = true;
        s.prev_gather_sec = g_sec;
        s.prev_trap_sec = t_sec;
        s.prev_gather_ready = g_act;
        s.prev_trap_ready = t_act;
    }
    // 自动熄屏(L1):档位非关 + 无操作超时;战斗/事件/确认弹屏中挂起计时
    // (§8.1,交互中突然黑屏会坑人),手动长按下不受此限
    uint8_t smode = sleep_mode();
    bool in_dialog = s.page == PG_COMBAT || s.page == PG_EVENT ||
                     s.page == PG_CONFIRM;
    if (!s.screen_off) {
        uint16_t to_s = k_scr_off_s[scr_off_level()];
        if (to_s > 0 && !in_dialog &&
            now_ms - s.last_input_ms >= (uint32_t)to_s * 1000u)
            screen_off_now();
    }
    // L2 浅睡 / L3 深睡(§8.1):熄屏之后叠加计时,弹屏同样挂起;
    // 请求由 key_task 消费(打盹循环 / 深睡序列),tick 只置位。
    if (!in_dialog) {
        uint32_t idle = now_ms - s.last_input_ms;
        if (!s.deep_req) {
            if ((smode & 1u) && idle >= DR_LIGHT_SLEEP_S * 1000u)
                s.sleep_req = true;    // L2:先熄屏后浅睡(L1 到时已在上面触发)
            if (smode >= 2u && idle >= DR_DEEP_SLEEP_S * 1000u)
                s.deep_req = true;     // L3:到点存档+深睡(浅睡打盹中也会被
        }                              // key_task 用同条件接管)
    } else {
        s.sleep_req = false;           // 弹屏中撤回浅睡请求(交互优先)
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
// tab 启用表:小屋/≡ 常开;村庄 = 森林剧情解锁;荒野 = 罗盘(原版
// room.js updateStoresView: 买到罗盘即 Path.openPath,I27——远征系统
// 已实装,解锁节奏与原版一致)
static bool forest_open(void) {
    return (s.game.flags & ((uint64_t)1u << DR_FLAG_FOREST)) != 0;
}
static bool compass_held(void) {
    return (s.game.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) != 0;
}
// 星舰页签门:踏上坠毁星舰格解锁(原版 features.location.spaceShip)
static bool ship_open(void) {
    return (s.game.flags & ((uint64_t)1u << DR_FLAG_SHIP_OPEN)) != 0;
}
static bool nav_tab_enabled(int i) {
    switch (i) {
        case 1: return forest_open();
        case 2: return compass_held();
        case 3: return ship_open();
        default: return true;
    }
}

// 导入页"确定":确认当前草稿位并前进。输到 8 字符解帧头得总长;输满自动
// 校验:通过 = 核心档入账(新世界/新事件,离线不结算)+ 存档;失败 = 失败态。
static void import_confirm_char(void) {
    if (s.imp_total > 0 && s.imp_pos >= s.imp_total) return;   // 已满,等自动校验
    s.imp_buf[s.imp_pos++] = DR_CODE_B32[s.imp_draft];
    s.imp_draft = 0;
    if (s.imp_total == 0 && s.imp_pos >= 8) {
        int t = (int)dr_savecode_expect_len(s.imp_buf, (size_t)s.imp_pos);
        if (t == 0) { s.imp_failed = true; return; }           // 魔数/版本不符
        s.imp_total = t;
    }
    if (s.imp_total > 0 && s.imp_pos >= s.imp_total) {
        dr_code_core_t core;
        if (dr_savecode_import(s.imp_buf, (size_t)s.imp_total, &core)) {
            uint32_t seed = (uint32_t)esp_timer_get_time() ^ 0x5EED;
            dr_savecode_apply(&core, &s.game, seed, dr_port_now_ts());
            dr_world_gen(&s_world, s.game.map_seed);   // 码不保地图,新种子新图
            s_log_cnt = 0;
            log_push("存档码恢复成功");
            toast("恢复成功");
            s.save_pending = true;
            page_goto(PG_HOME);
        } else {
            s.imp_failed = true;   // CRC/解压失败:码有误或不完整
        }
    }
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev, uint32_t ev_ms) {
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN && btn != BSP_BTN_OK) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s.boot_ms < 1500u) return;  // 开机防抖
    if (s.screen_off) {                      // 任意键唤醒,唤醒键吞掉不触发游戏动作
        s.last_input_ms = now_ms;            // 唤醒本身也算一次输入
        screen_wake();
        return;
    }
    // 双击下键 = 手动熄屏(I38:原"长按下键熄屏"与 I37 连发滚动抢手势——
    // 组件 1500ms 先发 LONG(熄屏)后发 HOLD(连发),按住下键滚动必被熄屏打断)。
    // 导入页例外:双击下键 = 随时退出导入(拍板"不阻断退出";导入是短时
    // 操作,手动熄屏在此让位)
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_DOUBLE) {
        if (s.page == PG_IMPORT) {
            page_goto(PG_SETTINGS);
            return;
        }
        screen_off_now();
        return;
    }
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_LONG && ev != BSP_BTN_HOLD) return;
    // 幽灵键双闸(I33):按下沿看"渲染/flush 后静默期"——渲染电流尖峰制造的
    // 幽灵按键紧贴渲染出现;长按与连调看"实测电压是否仍在该键窗口内"——真实
    // 按住读数居窗口正中,轨塌/漏电型幽灵骑在边界,一测即穿帮。
    // 被拒的键不重置熄屏计时(否则幽灵风暴会让自动熄屏永远不触发)。
    // I34:静默窗改按"事件产生时刻"(入队时间戳)判定——原按处理时刻,
    // 冷却重绘期排队键被拖到 flush 后才处理,真实按键反被当幽灵吞掉。
    if (ev == BSP_BTN_PRESS) {
        int32_t dt = (int32_t)(ev_ms - s.last_render_ms);
        if (dt >= 0 && dt < 60) {
            ESP_LOGI(TAG, "diag drop btn=%d dt=%ld", (int)btn, (long)dt);
            return;
        }
    } else if (!bsp_button_mv_in_window(btn)) {
        return;
    }
    s.last_input_ms = now_ms;                // 通过鉴别的按键才重置熄屏计时

    // 连调:长按期间周期事件——调节态按住上/下自动步进(节流 ~10 步/s;
    // 组件原始周期 20ms 过快)。其他场景忽略(翻页/确定不做连发)。
    if (ev == BSP_BTN_HOLD) {
        static uint32_t s_last_hold_ms = 0;
        if (btn == BSP_BTN_OK) return;
        if (s.page == PG_EXPORT || s.page == PG_IMPORT) return;   // 换字/翻页不连发
        if (now_ms - s_last_hold_ms < 100u) return;
        s_last_hold_ms = now_ms;
        if (s.page == PG_VILLAGE && s.village_adj >= 0) {
            int16_t d = (btn == BSP_BTN_UP) ? 1 : -1;
            (void)dr_rules_job_assign(&s.game, (uint8_t)s.village_adj, d);
            s.save_pending = true;
            s.dirty = true;
            return;
        }
        if (s.page == PG_MAP && !s.game.in_wilderness && s.outfit_adj >= 0) {
            static const uint8_t outfit_res[6] = {
                DR_RES_FOOD, DR_RES_MEDICINE, DR_RES_BULLETS,
                DR_RES_ENERGY_CELL, DR_RES_GRENADE, DR_RES_BOLAS
            };
            int16_t d = (btn == BSP_BTN_UP) ? 1 : -1;
            (void)dr_world_outfit_add(&s_world, &s.game,
                                      outfit_res[s.outfit_adj], d);
            s.save_pending = true;
            s.dirty = true;
            return;
        }
        // 主页日志浏览(T8):选中态按住上/下连续滚窗(节流同 100ms);
        // 仅聚焦未选中时连发无意义,直接吞掉
        if (s.page == PG_HOME && s.log_focus && !s.log_browse) return;
        if (s.page == PG_HOME && s.log_browse) {
            int max_off = (s_log_cnt > LOG_VIS) ? s_log_cnt - LOG_VIS : 0;
            if (btn == BSP_BTN_UP && s.log_off > 0) {
                s.log_off--;
                s.dirty = true;
            } else if (btn == BSP_BTN_DOWN && s.log_off < max_off) {
                s.log_off++;
                s.dirty = true;
            }
            return;
        }
        // 光标长按连发(I37):常规列表/弹窗按住上/下逐行连走,免逐下按
        // (调节态的数量连发已在上面两个分支处理;节流同 100ms)
        if (btn == BSP_BTN_UP) focus_move(-1);
        else focus_move(1);
        s.dirty = true;
        return;
    }

    if (ev == BSP_BTN_PRESS) {
        // 存档码导出页:上/下翻页,确定=下一页(尾页=返回设置)
        if (s.page == PG_EXPORT) {
            if (btn == BSP_BTN_UP) {
                if (s.code_page > 0) s.code_page--;
            } else if (btn == BSP_BTN_DOWN) {
                if (s.code_page + 1 < s.code_pages) s.code_page++;
            } else {
                if (s.code_page + 1 < s.code_pages) s.code_page++;
                else page_goto(PG_SETTINGS);
            }
            s.dirty = true;
            return;
        }
        // 存档码导入页:上/下换草稿字符,确定=确认前进;失败态确定=清空重来
        if (s.page == PG_IMPORT) {
            if (s.imp_failed) {
                if (btn == BSP_BTN_OK) {
                    s.imp_pos = 0;
                    s.imp_total = 0;
                    s.imp_draft = 0;
                    s.imp_failed = false;
                }
            } else if (btn == BSP_BTN_UP) {
                s.imp_draft = (s.imp_draft + 31) % 32;
            } else if (btn == BSP_BTN_DOWN) {
                s.imp_draft = (s.imp_draft + 1) % 32;
            } else if (btn == BSP_BTN_OK) {
                import_confirm_char();
            }
            s.dirty = true;
            return;
        }
        // 幽灵键窗已在函数头统一鉴别(I33),这里直接进动作分发
        // 出发整备调配:上/下=±1(长按=±5),确定=完成退出(与村庄人数调节
        // 同款;原"确定切下一项"在 6 携带行下退出要连按 6 下——真机 I36)
        if (s.page == PG_MAP && s.outfit_adj >= 0) {
            static const uint8_t outfit_res[6] = {
                DR_RES_FOOD, DR_RES_MEDICINE, DR_RES_BULLETS,
                DR_RES_ENERGY_CELL, DR_RES_GRENADE, DR_RES_BOLAS
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
                                         outfit_res[s.outfit_adj], d)) {
                    const char *m = d > 0 ? "背袋满了或库存不足" : "已经没有携带了";
                    log_push(m);
                    toast(m);
                }
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
                if (!dr_rules_job_assign(&s.game, (uint8_t)s.village_adj, d)) {
                    const char *m = d > 0 ? "没有闲着的人了" : "这个岗位已经没人了";
                    log_push(m);
                    toast(m);
                }
                s.save_pending = true;
                s.dirty = true;
            }
            return;
        }
        // 主页:导航栏(小屋/村庄/荒野/星舰/≡)是焦点首区。动作区顶端向上进入导航;
        // 导航内 UP 在启用的 tab 间循环(未启用跳过)、DOWN 回到动作区首行;
        // OK 切页
        if (s.page == PG_HOME && s.nav_focus >= 0) {
            if (btn == BSP_BTN_UP) {
                do {
                    s.nav_focus = (s.nav_focus + 4) % 5;
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
                    case 2: if (compass_held()) page_goto(PG_MAP);   // 罗盘开荒野(I27)
                            else { log_push("荒野尚未解锁"); go = false; }
                            break;
                    case 3: if (ship_open()) page_goto(PG_SHIP);
                            else { log_push("尚未发现星舰"); go = false; }
                            break;
                    case 4: page_goto(PG_SETTINGS); break;
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
        // 主页日志窗(T8):下过末行动作 → 聚焦日志窗(金描边);
        // 聚焦态 OK=选中(滚窗可滚)/上=回动作区;选中态 OK=取消选中
        if (s.page == PG_HOME && s.log_browse) {
            if (btn == BSP_BTN_OK) {
                s.log_browse = false;   // 取消选中,保持聚焦
                s.dirty = true;
                return;
            }
            int max_off = (s_log_cnt > LOG_VIS) ? s_log_cnt - LOG_VIS : 0;
            if (btn == BSP_BTN_UP && s.log_off > 0) {
                s.log_off--;
                s.dirty = true;
            } else if (btn == BSP_BTN_DOWN && s.log_off < max_off) {
                s.log_off++;
                s.dirty = true;
            }
            return;
        }
        if (s.page == PG_HOME && s.log_focus) {
            if (btn == BSP_BTN_OK) {
                s.log_browse = true;   // 选中:滚窗生效+右缘滚动条
                s.dirty = true;
                return;
            }
            if (btn == BSP_BTN_UP) {
                s.log_focus = false;   // 上=回动作区末行
                s.dirty = true;
                return;
            }
            return;   // 聚焦态下键无动作(已是焦点链末端)
        }
        if (s.page == PG_HOME && btn == BSP_BTN_DOWN && s.nav_focus < 0 &&
            s.focus == home_lines() - 1) {
            s.log_focus = true;   // 下过末行 → 聚焦日志窗
            s.dirty = true;
            return;
        }
        // 村庄页职业区滚动:窗口 5 行。上/下滚条件按"区域"而非精确行号——
        // 职业行可能全部锁定(光标被跳过,永远落不到窗口顶/末行),
        // 焦点在顶部区(≤3)按上=上翻,在底部区(≥7)按下=下翻,保证双向可达
        if (s.page == PG_VILLAGE && s.village_adj < 0 &&
            !s.game.in_wilderness) {
            if (btn == BSP_BTN_UP && s.job_scroll > 0 && s.focus <= 3) {
                s.job_scroll--;
                s.dirty = true;
                return;
            }
            if (btn == BSP_BTN_DOWN && s.focus >= 7 &&
                s.job_scroll + 5 < DR_JOB_KIND_COUNT) {
                s.job_scroll++;
                s.dirty = true;
                return;
            }
        }
        // 制造页滚动:窗口 8 行,同村庄按区域滚动(已有/不可造行被光标跳过时
        // 仍可双向滚动:顶部区(≤1)或返回行按上=上翻,末行/返回行按下=下翻)
        if (s.page == PG_CRAFT) {
            if (btn == BSP_BTN_UP && s.craft_scroll > 0 &&
                (s.focus <= 1 || s.focus >= s.craft_row_count)) {
                s.craft_scroll--;
                s.dirty = true;
                return;
            }
            if (btn == BSP_BTN_DOWN &&
                s.focus >= s.craft_row_count - 1 &&
                s.craft_scroll + s.craft_row_count < s.craft_total) {
                s.craft_scroll++;
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
        // 存档码两页的长按语义(拍板):导出=回第 1 页;导入=退格(失败态=退出)
        if (s.page == PG_EXPORT) {
            s.code_page = 0;
            s.dirty = true;
            return;
        }
        if (s.page == PG_IMPORT) {
            if (s.imp_failed) {
                page_goto(PG_SETTINGS);
            } else if (s.imp_pos > 0) {
                s.imp_pos--;
                s.imp_draft = 0;
            }
            s.dirty = true;
            return;
        }
        // 村庄:调节中长按确定=完成;非调节态长按确定=返回(与其他列表页一致,
        // 2026-10-07 C 项:此前村庄是唯一不支持长按返回的列表页);
        // 调节中长按上/下 = ±5
        if (s.page == PG_VILLAGE) {
            if (btn == BSP_BTN_OK) {
                if (s.village_adj >= 0) {
                    s.village_adj = -1;
                    s.dirty = true;
                } else {
                    page_goto(PG_HOME);
                }
            } else if (s.village_adj >= 0) {
                int16_t d = (btn == BSP_BTN_UP) ? 5 : -5;
                if (!dr_rules_job_assign(&s.game, (uint8_t)s.village_adj, d)) {
                    const char *m = d > 0 ? "没有闲着的人了" : "这个岗位已经没人了";
                    log_push(m);
                    toast(m);
                }
                s.save_pending = true;
                s.dirty = true;
            }
            return;
        }
        // 荒野整备调节:长按确定=完成退出;长按上/下=±5(与村庄调节同款;
        // 此前长按分支不覆盖整备,提示承诺的"长按=5"实为不生效,真机反馈)
        if (s.page == PG_MAP && !s.game.in_wilderness && s.outfit_adj >= 0) {
            if (btn == BSP_BTN_OK) {
                s.outfit_adj = -1;
                s.dirty = true;
                return;
            }
            static const uint8_t outfit_res[6] = {
                DR_RES_FOOD, DR_RES_MEDICINE, DR_RES_BULLETS,
                DR_RES_ENERGY_CELL, DR_RES_GRENADE, DR_RES_BOLAS
            };
            int16_t d = (btn == BSP_BTN_UP) ? 5 : -5;
            if (!dr_world_outfit_add(&s_world, &s.game,
                                     outfit_res[s.outfit_adj], d)) {
                const char *m = d > 0 ? "背袋满了或库存不足" : "已经没有携带了";
                log_push(m);
                toast(m);
            }
            s.save_pending = true;
            s.dirty = true;
            return;
        }
        // 长按下键不再熄屏(I38:让位给连发滚动;手动熄屏=双击下键)
        if (btn != BSP_BTN_OK) return;
        // 危险操作的长按快捷路径:同样走确认弹窗(与按下确定一致)
        if (s.page == PG_SETTINGS && s.focus == 1) {
            s.confirm_from = 0;
            page_goto(PG_CONFIRM);
            return;
        }
        // 存档码行长按 = 导入(短按 = 导出,见 list_action);功能隐藏时无此行
#if DR_FEATURE_SAVECODE
        if (s.page == PG_SETTINGS && s.focus == 5) {
            page_goto(PG_IMPORT);
            return;
        }
#endif
        // 长按返回;事件/战斗/确认页禁用(必须显式选择);远征中禁用
        // (2026-10-07 A 项:此前远征中长按确定会瞬移回家——背包战利品
        // 悬空、再出发被清袋,规则上远征只有走回村或战死两个出口)
        switch (s.page) {
            case PG_BUILD:
            case PG_TRADE:
            case PG_SETTINGS:
            case PG_CRAFT:
            case PG_MAP:
                if (s.game.in_wilderness) {
                    toast("荒野中只能走回去");
                    return;
                }
                page_goto(PG_HOME);
                break;
            case PG_SHIP:   page_goto(PG_HOME); break;
            case PG_FAB:    page_goto(PG_SHIP); break;
            case PG_RUIN:   page_goto(PG_MAP);  break;
            default: break;
        }
    }
}

static void key_task(void *arg) {
    (void)arg;
    key_event_t ke;
    while (!s_key_quit) {
        // L3 深睡(§8.1 无操作 5min / §8.2 电量≤10% 硬保护):toast/日志先
        // 留 1.5s 可见窗,然后锁内一次性 存档→关屏→GPIO 唤醒配置→深睡。
        // esp_deep_sleep_start 不返回,锁永不释放,渲染任务天然被阻止
        // (bsp_display_prepare_deep_sleep 要求"已阻止 LVGL 刷屏"的满足方式)。
        if (s.deep_req) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (bsp_lvgl_lock(1000)) {
                save_all();
                ESP_LOGI(TAG, "进入深睡(存档已落盘,唤醒=重启走离线结算)");
                bsp_display_prepare_deep_sleep();
                bsp_battery_sleep();
                // GPIO0 低电平唤醒:松开态上拉 3300mV,按下最高档 595mV
                // (< 数字低电平),三键全部有效;ADC 在深睡中不可用,
                // 低电平判据只做"有键按下"级别的唤醒
                esp_deep_sleep_enable_gpio_wakeup(1ULL << DR_BTN_GPIO,
                                                  ESP_GPIO_WAKEUP_GPIO_LOW);
                esp_deep_sleep_start();
            }
            continue;
        }
        // L2 浅睡(§8.1):1s RTC 定时打盹循环(三键为 ADC 分压,浅睡中
        // ADC 无法采样/GPIO 唤醒不可用;打盹近似浅睡底电流)。醒来查队列
        // 与 ADC,有键即退——唤醒键随后走 screen_off 吞键分支(与 L1 同
        // 口径)。sleep_req 由 tick 持续重估,这里只消费;5min 深睡到点在
        // 打盹间隙判断(浅睡中 tick 不跑)。
        if (s.sleep_req) {
            if (!s.screen_off) screen_off_now();   // 浅睡必先灭屏(L1 关档时补)
            while (s.sleep_req && !s.deep_req && !s_key_quit) {
                esp_sleep_enable_timer_wakeup(1000000);
                esp_light_sleep_start();           // 全任务冻结,醒后时间连续
                if (uxQueueMessagesWaiting(s_key_queue) > 0 ||
                    (bsp_button_read_mv() >= 0 && bsp_button_read_mv() < 1900)) {
                    // 有键即醒。顺手清请求:唤醒键处理后到下一轮 tick 重估
                    // 之间,循环顶会再次读到 stale 的 true 而立刻熄屏回去
                    // (实测复现:唤醒后 backlight 停在 0)
                    s.sleep_req = false;
                    break;
                }
                if (sleep_mode() >= 2u &&
                    (uint32_t)(esp_timer_get_time() / 1000) - s.last_input_ms >=
                        DR_DEEP_SLEEP_S * 1000u) {
                    s.deep_req = true;             // 浅睡中到点:转深睡
                    break;
                }
            }
        }
        if (xQueueReceive(s_key_queue, &ke, pdMS_TO_TICKS(200)) == pdTRUE) {
            // I33 取证:每个按键事件附当时分压读数(仅日志,零行为变更;定位后撤)
            ESP_LOGI(TAG, "diag key btn=%d ev=%d mv=%d page=%d",
                     (int)ke.btn, (int)ke.ev, bsp_button_read_mv(), (int)s.page);
            if (bsp_lvgl_lock(250)) {
                handle_key(ke.btn, ke.ev, ke.ms);
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
    s.fight_return = PG_MAP;   // 战斗结束默认回荒野页(执行者战临时改 PG_SHIP)
    s.last_dir = -1;           // 方向记忆(首次进荒野前为空,I35)
    s.boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s.last_input_ms = s.boot_ms;   // 熄屏计时与开机防抖同基准

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
    // 全局节拍门起步 3 分钟(原版 scheduleNextEvent 首拍 3~6 分钟);
    // 选项掷点流按地图种子派生,会话内确定。
    s.ev_sess.next_ok_ts = dr_port_now_ts() + 180u;
    dr_rng_seed(&s.ev_rng, (uint32_t)s.game.map_seed * 31u + 7u);
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
                    char line[80];   // 五段数值全满 68 字节,64 会截尾
                    snprintf(line, sizeof(line),
                             "离线:木+%lu 毛+%lu 肉+%lu 革+%lu 干+%lu",
                             (unsigned long)oy.wood, (unsigned long)oy.fur,
                             (unsigned long)oy.meat, (unsigned long)oy.leather,
                             (unsigned long)oy.food);
                    log_push(line);
                }
                if (oy.stolen_wood || oy.stolen_fur || oy.stolen_meat) {
                    char line[80];   // 三段数值全满 63 字节,与上行对齐留量
                    snprintf(line, sizeof(line),
                             "离线:库房被偷 木%lu 毛%lu 肉%lu",
                             (unsigned long)oy.stolen_wood,
                             (unsigned long)oy.stolen_fur,
                             (unsigned long)oy.stolen_meat);
                    log_push(line);
                }
                if (oy.fire_out) log_push("回来时火已经熄了");
            }
    }

    // 世界(种子派生,重启同图)与远征中断兜底:断电按死亡处理(物资已扣)
    dr_world_gen(&s_world, s.game.map_seed);
    // 斥候 Perk:开机确定性揭示一片(迷雾会话态,同种子每次开机同一片)
    if ((s.game.flags & ((uint64_t)1u << DR_FLAG_PERK_SCOUT)) &&
        dr_world_apply_map(&s_world, &s.game))
        log_push("侦查的本事让你记起了荒野的一角");
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
    key_event_t ke = { .btn = btn, .ev = ev,
                       .ms = (uint32_t)(esp_timer_get_time() / 1000) };
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

// 模拟器诊断:注入"远征中"状态并直达荒野页(仅主机构建使用)。存档口径
// 必清 in_wilderness("远征中断电=死亡"),enter 后注入绕开该兜底,供荒野页/
// 战斗页截图与断言(SIM_WILD)。
void dr_debug_set_wild(uint8_t x, uint8_t y) {
    s.game.in_wilderness = 1;
    s.game.food = 30; s.game.water = 30;   // 长走位截图用(战斗触发需多步)
    s.game.hero_hp = s.game.hero_hp_max = 10;
    s.game.hero_x = x; s.game.hero_y = y;
    s.game.res[DR_RES_FOOD] = 40;
    // 点亮主角周围 9×9(纹样/地形截图验证需要视野内有内容)
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int cx = (int)x + dx, cy = (int)y + dy;
            if (cx < 0 || cy < 0 || cx >= DR_WORLD_SIZE || cy >= DR_WORLD_SIZE)
                continue;
            uint32_t i = (uint32_t)cy * DR_WORLD_SIZE + cx;
            s_world.seen[i / 8] |= (uint8_t)(1u << (i % 8));
        }
    s.page = PG_MAP;
    s.focus = 0;
    s.dirty = true;
}

// 模拟器诊断:直达事件弹窗(仅测试钩子;事件表按"最小 id 优先"确定性挑选,
// 按键脚本等不到低频事件,这里直接指定表下标——SIM_EVENT 注入,配合
// SIM_PERSIST 造档可实测任意事件的选择链)。
void dr_debug_force_event(uint16_t idx) {
    uint16_t count = 0;
    dr_events_table(&count);
    if (idx >= count) return;
    s.ev_idx = idx;
    s.ev_from = PG_HOME;   // 解决后回主页(村庄事件可改由按键先行切页)
    page_goto(PG_EVENT);
}

// 模拟器诊断:星舰页直达(仅主机构建;SIM_SHIP=exec,hull,thr,alloy)。
// 注入页签旗标与船态,供星舰页/制造台/升空序列截图与按键实测。
void dr_debug_set_ship(uint8_t exec, uint8_t hull, uint8_t thr, uint32_t alloy) {
    s.game.flags |= (uint64_t)1u << DR_FLAG_SHIP_OPEN;
    s.game.exec_state = exec;
    s.game.ship_hull = hull;
    s.game.ship_thrusters = thr;
    s.game.res[DR_RES_ALIEN] = alloy;
    page_goto(PG_SHIP);
}

// 模拟器诊断:整备页直达(仅主机构建;SIM_OUTFIT=1:置罗盘开荒野页签,
// 未远征态 = 出发整备视图),供整备调节按键实测(I36)。
void dr_debug_set_outfit(void) {
    s.game.flags |= (uint64_t)1u << DR_FLAG_COMPASS;
    page_goto(PG_MAP);
}

// 模拟器诊断:导入页逐字符注入(SIM_TYPE_CODE)——按键脚本输百字符级
// 存档码会超键位表容量,跳过"上/下换字"的按键路径直接喂字符流,实测
// 帧头预读/自动校验/入账全链(换字按键路径由 8 键手测覆盖)。注入时若
// 不在导入页(enter 后页面还在标题页),先自行切入。
void dr_debug_type_code(const char *code) {
    if (s.page != PG_IMPORT) page_goto(PG_IMPORT);
    for (; *code; code++) {
        const char *p = strchr(DR_CODE_B32, *code);
        if (!p) continue;
        s.imp_draft = (int)(p - DR_CODE_B32);
        import_confirm_char();
        if (s.imp_failed) return;
        if (s.imp_total > 0 && s.imp_pos >= s.imp_total) return;   // 输满/已入账
    }
}

// 模拟器诊断:打印对象树关键状态(仅主机构建使用)
void dr_debug_dump(void) {
    ESP_LOGI(TAG, "dump page=%d focus=%d adj=%d wood=%lu pop=%u fire=%u hero=%u,%u dir=%d wild=%u",
             (int)s.page, s.focus, (int)s.outfit_adj,
             (unsigned long)s.game.res[DR_RES_WOOD],
             s.game.population, s.game.fire_lv,
             s.game.hero_x, s.game.hero_y, (int)s.last_dir,
             s.game.in_wilderness);
}

// 模拟器诊断:轮播下一页(SIM_PAGE_SWEEP 用)
void dr_sweep_next(void) {
    static const page_t order[] = {
        PG_TITLE, PG_HOME, PG_BUILD, PG_VILLAGE, PG_MAP, PG_RUIN,
        PG_COMBAT, PG_TRADE, PG_SETTINGS, PG_EVENT, PG_CONFIRM,
        PG_SHIP, PG_FAB, PG_LIFTOFF, PG_ENDING, PG_EXPORT, PG_IMPORT,
    };
    static int i = 0;
    page_goto(order[i % (sizeof(order) / sizeof(order[0]))]);
    if (s.page == PG_LIFTOFF && s.lift_total < 8) s.lift_total = 8;  // 轮播兜底
    if (s.page == PG_EVENT) {
        uint16_t c = 0;
        dr_events_table(&c);
        if (c == 0) {   // 空表:M6 灌装前事件页无可渲染内容,跳过
            i++;
            page_goto(order[i % (sizeof(order) / sizeof(order[0]))]);
        } else {
            s.ev_idx = 0;
        }
    }
    i++;
}
