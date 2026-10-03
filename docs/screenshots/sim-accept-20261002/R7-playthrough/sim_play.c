// sim_play.c —— 《小黑屋》玩家路径全自动回放(状态感知按键剧本)。
// 基于 sim_pace 垫片;项目源码未动。三条腿:
//   1) 快照:垫片在 dr_port_save 时打包内存镜像,驱动器随时解包读取真实局内状态;
//   2) 焦点模型:驱动器复刻应用的 row_enabled/focus_move/滚动规则,精确算出按键次数;
//   3) 快进:长等待停泊在无周期重绘的页面(建造页),主循环去 usleep 只推虚拟时钟。
// 每个动作后校验快照并截图;CHECK 失败即打印诊断退出。
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "darkroom_app.h"
#include "dr_config.h"
#include "dr_port.h"
#include "dr_rules.h"
#include "dr_state.h"
#include "dr_world.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#define SIM_W 240
#define SIM_H 320

static volatile int64_t s_vtime_us = 1000000;
int64_t esp_timer_get_time(void) { return s_vtime_us; }

struct sim_queue {
    int item_size, head, count, capacity;
    uint8_t *storage;
};
static volatile long s_q_sent, s_q_recvd;   // 按键投递/消费计数(press 排空用)
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct sim_queue *q = calloc(1, sizeof(*q));
    q->item_size = (int)item_size; q->capacity = (int)length;
    q->storage = calloc(length, item_size);
    return (QueueHandle_t)q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    (void)wait;
    struct sim_queue *s = (struct sim_queue *)q;
    if (s->count >= s->capacity) return pdFALSE;
    int tail = (s->head + s->count) % s->capacity;
    memcpy(s->storage + tail * s->item_size, item, (size_t)s->item_size);
    s->count++;
    s_q_sent++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) {
    struct sim_queue *s = (struct sim_queue *)q;
    if (s->count == 0) { usleep(1000); return pdFALSE; }
    memcpy(out, s->storage + s->head * s->item_size, (size_t)s->item_size);
    s->head = (s->head + 1) % s->capacity;
    s->count--;
    s_q_recvd++;
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    return (UBaseType_t)((struct sim_queue *)q)->count;
}
void vQueueDelete(QueueHandle_t q) { free(((struct sim_queue *)q)->storage); free(q); }

typedef void (*task_fn_t)(void *);
pthread_t s_threads[4]; static int s_thread_cnt;
BaseType_t xTaskCreate(task_fn_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, void **handle) {
    (void)name; (void)stack; (void)prio;
    if (s_thread_cnt >= 4) return pdPASS == 0;
    pthread_create(&s_threads[s_thread_cnt], NULL, (void *(*)(void *))fn, arg);
    s_thread_cnt++;
    return pdPASS;
}
void vTaskDelete(void *t) { (void)t; }
void vTaskDelay(TickType_t ms) { usleep((useconds_t)ms * 1000); }

static dr_save_image_t s_save_img;
static bool s_save_valid;
static volatile long s_save_seq;   // 存档序号(k_ok 后等待新存档落账用)
static pthread_mutex_t s_snap_mtx = PTHREAD_MUTEX_INITIALIZER;
int dr_port_storage_init(void) { return 0; }
uint32_t dr_port_now_ts(void) { return (uint32_t)(s_vtime_us / 1000000); }
int dr_port_save(const dr_game_t *g) {
    dr_save_image_t img;
    dr_state_pack(g, &img);
    pthread_mutex_lock(&s_snap_mtx);
    s_save_img = img;
    s_save_valid = true;
    s_save_seq++;
    pthread_mutex_unlock(&s_snap_mtx);
    return 0;
}
static long save_seq(void) {
    pthread_mutex_lock(&s_snap_mtx);
    long v = s_save_seq;
    pthread_mutex_unlock(&s_snap_mtx);
    return v;
}
int dr_port_load(dr_game_t *g, bool *out_loaded) {
    (void)g;
    *out_loaded = false;   // 回放永远从新档开始(内存档)
    return 0;
}
int dr_port_log_save(const void *blob, size_t len) { (void)blob; (void)len; return 0; }
int dr_port_log_load(void *buf, size_t cap, size_t *out_len) { (void)buf; (void)cap; *out_len = 0; return 0; }

static pthread_mutex_t s_lvgl_mtx = PTHREAD_MUTEX_INITIALIZER;
bool sim_lvgl_lock(int timeout_ms) {
    (void)timeout_ms;
    return pthread_mutex_lock(&s_lvgl_mtx) == 0;
}
void sim_lvgl_unlock(void) { pthread_mutex_unlock(&s_lvgl_mtx); }

static uint8_t s_fb[SIM_W * SIM_H * 4];
static volatile int64_t s_flush_vus;   // 最近一次真实渲染(flush_cb)的 vtime
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    (void)area; (void)px;
    s_flush_vus = s_vtime_us;
    lv_display_flush_ready(disp);
}
static const char *s_out = "";
static void write_bmp(const char *path) {
    uint32_t row_size = (uint32_t)SIM_W * 3, data_size = row_size * SIM_H;
    uint8_t h[54] = {0};
    uint32_t fs = 54 + data_size;
    memcpy(h + 2, &fs, 4);
    h[10] = 54; h[14] = 40;
    int32_t w = SIM_W, hh = SIM_H;
    memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    h[26] = 1; h[28] = 24;
    memcpy(h + 34, &data_size, 4);
    uint8_t *rows = malloc(data_size);
    for (int y = 0; y < SIM_H; y++) {
        uint8_t *dst = rows + (uint32_t)(SIM_H - 1 - y) * row_size;
        for (int x = 0; x < SIM_W; x++) {
            const uint8_t *p = s_fb + ((uint32_t)y * SIM_W + x) * 4;
            dst[x * 3 + 0] = p[0]; dst[x * 3 + 1] = p[1]; dst[x * 3 + 2] = p[2];
        }
    }
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, 54, f); fwrite(rows, 1, data_size, f); fclose(f);
    free(rows);
}

// ================= 基础驱动 =================
static int64_t s_t0_us;
static int s_shot_n;
static dr_game_t G;   // 最新快照

static void frames(int n) {
    for (int i = 0; i < n; i++) {
        s_vtime_us += 30000;
        lv_tick_inc(30);
        sim_lvgl_lock(1000);
        lv_timer_handler();
        sim_lvgl_unlock();
        usleep(1000);
    }
}
static double vt_s(void) { return (double)(s_vtime_us - s_t0_us) / 1e6; }

static void shot(const char *name) {
    char p[160];
    snprintf(p, sizeof(p), "%s%03d-%s.bmp", s_out, s_shot_n++, name);
    sim_lvgl_lock(1000);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    sim_lvgl_unlock();
    write_bmp(p);
    printf("[%7.1fs] shot %s\n", vt_s(), p);
}

static bool snap(void) {
    pthread_mutex_lock(&s_snap_mtx);
    bool ok = s_save_valid;
    dr_save_image_t img = s_save_img;
    pthread_mutex_unlock(&s_snap_mtx);
    uint16_t ver;
    if (ok) ok = dr_state_unpack(&img, &G, &ver);
    return ok;
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("[CHECK FAIL @%.1fs] ", vt_s()); printf(__VA_ARGS__); printf("\n"); \
        printf("  wood=%u fur=%u meat=%u scales=%u teeth=%u cloth=%u leather=%u pop=%u\n" \
               "  fire=%u temp=%u builder=%u hut=%u trap=%u idle=%u\n", \
               G.res[DR_RES_WOOD], G.res[DR_RES_FUR], G.res[DR_RES_MEAT], \
               G.res[DR_RES_SCALES], G.res[DR_RES_TEETH], G.res[DR_RES_CLOTH], \
               G.res[DR_RES_LEATHER], G.population, G.fire_lv, G.temp_lv, \
               G.builder_lv, G.building_lv[DR_BLD_HUT], \
               G.building_lv[DR_BLD_TRAP], dr_rules_job_idle(&G)); \
        { char cp[96]; snprintf(cp, sizeof(cp), "%sCRASH.bmp", s_out); \
          sim_lvgl_lock(1000); lv_obj_invalidate(lv_screen_active()); \
          lv_refr_now(NULL); sim_lvgl_unlock(); write_bmp(cp); } \
        exit(1); \
    } \
} while (0)

static long s_trace;   // >0 时逐键打印轨迹
static volatile int64_t s_man_refr_vus;   // 最近一次手动整页刷新的 vtime(见 refresh_fb)
static void press(bsp_btn_t b, bsp_btn_ev_t ev) {
    // 渲染静默期精确免疫:60ms 幽灵窗判虚拟时钟,而一切渲染必经 flush_cb——
    // 记录其 vtime,按键消费时刻保证与最近渲染拉开 ≥90ms(虚拟),对心跳
    // 渲染/逐秒倒计时重绘/key_task 渲染/手动刷帧全部确定性安全。
    for (int i = 0; i < 30 && s_vtime_us - s_flush_vus < 90000; i++)
        frames(1);
    long want = s_q_sent + 1;
    darkroom_app_key(b, ev);
    for (int i = 0; i < 1000 && s_q_recvd < want; i++) usleep(1000);
    usleep(20000);   // 给 handle_key 之后的 render 收尾
    frames(6);
}
static void k_ok(void)   { press(BSP_BTN_OK,   BSP_BTN_PRESS); }
static void k_down(void) { press(BSP_BTN_DOWN, BSP_BTN_PRESS); }
static void k_up(void)   { press(BSP_BTN_UP,   BSP_BTN_PRESS); }
static void k_long(void) { press(BSP_BTN_OK,   BSP_BTN_LONG); }
// 动作键:按下后等到新存档落账再返回(渲染先于存稿,20ms 不够稳)
static void k_ok_saved(void) {
    long s0 = save_seq();
    k_ok();
    for (int i = 0; i < 800 && save_seq() <= s0; i++) usleep(1000);
}

// 快进:停泊页须无周期重绘(建造/贸易/设置/制造),去 usleep 只推虚拟时钟
static void fast_wait(double seconds) {
    int64_t target = s_vtime_us + (int64_t)(seconds * 1e6);
    while (s_vtime_us < target) {
        s_vtime_us += 30000;
        lv_tick_inc(30);
        sim_lvgl_lock(1000);
        lv_timer_handler();
        sim_lvgl_unlock();
    }
}

// ================= 页面/焦点模型(复刻 darkroom_app 规则) =================
typedef enum { PG_TITLEX, PG_HOMEX, PG_BUILDX, PG_VILLAGEX, PG_TRADEX,
               PG_SETTINGSX, PG_CRAFTX, PG_MAP_OUTFITX, PG_MAP_EXPX,
               PG_COMBATX, PG_RUINX } ppage_t;
static ppage_t P;
static int F;
static int job_scroll;
static int craft_scroll;

static int home_lines(void) {
    int n = 2;
    if (G.building_lv[DR_BLD_WORKSHOP] > 0) n++;
    if (G.building_lv[DR_BLD_TRADE_POST] > 0) n++;
    return n;
}
static bool home_en(int idx) { (void)idx; return true; }

static bool build_en(int idx) {
    if (idx == DR_BLD_KIND_COUNT) return true;
    if (idx > DR_BLD_KIND_COUNT) return false;
    if (G.temp_lv <= DR_TEMP_COLD) return false;
    if (!dr_rules_can_build(&G, (uint8_t)idx)) return false;
    dr_bld_cost_t c = dr_building_cost((uint8_t)idx, G.building_lv[idx]);
    if (c.wood == 0xFFFFFFFFu) return false;
    return G.res[DR_RES_WOOD] >= c.wood && G.res[DR_RES_FUR] >= c.fur &&
           G.res[DR_RES_MEAT] >= c.meat;
}
static int build_lines(void) { return DR_BLD_KIND_COUNT + 1; }

static int village_job_of(int row) {
    int job = (row - 3) + job_scroll;
    return (row >= 3 && row <= 7 && job >= 0 && job < DR_JOB_KIND_COUNT) ? job : -1;
}
static bool village_en(int idx) {
    if (idx == 0 || idx == 8) return true;
    if (idx == 1) return G.building_lv[DR_BLD_TRAP] > 0;
    if (idx == 2) return false;
    int job = village_job_of(idx);
    return job >= 0 && dr_rules_job_unlocked(&G, (uint8_t)job);
}
static int village_lines(void) { return 9; }

static bool trade_afford(int idx) {
    if (G.building_lv[DR_BLD_TRADE_POST] == 0) return false;
    switch (idx) {
        case 0: return G.res[DR_RES_FUR] >= 150;
        case 1: return G.res[DR_RES_FUR] >= 300;
        case 2: return G.res[DR_RES_FUR] >= 150 && G.res[DR_RES_SCALES] >= 50;
        case 3: return G.res[DR_RES_FUR] >= 200 && G.res[DR_RES_TEETH] >= 50;
        case 4: return G.res[DR_RES_FUR] >= 300 && G.res[DR_RES_SCALES] >= 50 &&
                       G.res[DR_RES_TEETH] >= 50;
        case 5: return G.res[DR_RES_SCALES] >= 10;
        case 6: return G.res[DR_RES_SCALES] >= 50 && G.res[DR_RES_TEETH] >= 30;
        case 7: return !(G.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) &&
                       G.res[DR_RES_FUR] >= 400 && G.res[DR_RES_SCALES] >= 20 &&
                       G.res[DR_RES_TEETH] >= 10;
        default: return false;
    }
}
static bool trade_en(int idx) { return idx == 8 || trade_afford(idx); }
static int trade_lines(void) { return 9; }

static bool settings_en(int idx) { return idx != 3 && idx != 4; }
static int settings_lines(void) { return 6; }

// 地图整备态 7 行 / 远征态 5 行 / 战斗 4 行
static bool map_outfit_en(int idx) {
    if (idx == 0 || idx == 6) return true;
    if (idx == 1) return G.res[DR_RES_MEDICINE] > 0;
    if (idx == 2) return G.res[DR_RES_BULLETS] > 0;
    if (idx == 5) return G.food > 0;
    return false;   // 3武器/4护甲只读
}
static int map_outfit_lines(void) { return 7; }
static bool map_exp_en(int idx) {
    if (idx < 4) return true;          // 东南西北
    return G.food > 0;                 // 吃干肉
}
static int map_exp_lines(void) { return 5; }
static bool combat_en(int idx) { (void)idx; return true; }
static int combat_lines(void) { return 4; }

static uint8_t craft_vis[DR_CRAFT_KIND_COUNT];
static int craft_total, craft_rows;
static void craft_recalc(void) {
    craft_total = 0;
    for (int c = 0; c < DR_CRAFT_KIND_COUNT; c++)
        if (dr_rules_craft_visible(&G, (uint8_t)c)) craft_vis[craft_total++] = (uint8_t)c;
    craft_rows = (craft_total < 8) ? craft_total : 8;
    if (craft_scroll + craft_rows > craft_total)
        craft_scroll = (craft_total > craft_rows) ? craft_total - craft_rows : 0;
}
static bool craft_en(int row) {
    if (row >= craft_rows) return true;
    if (row < 0 || craft_scroll + row >= craft_total) return false;
    return dr_rules_craft_ready(&G, craft_vis[craft_scroll + row]);
}
static int craft_lines(void) { return craft_rows + 1; }

static void sim_move(bool (*en)(int), int lines, int dir) {
    for (int i = 0; i < lines; i++) {
        F = (F + dir + lines) % lines;
        if (en(F)) return;
    }
}

// ================= 闭环视觉导航:读帧缓冲里的金色箭头行 =================
// 单发丢键会级联脱轨(时序免疫已做,仍留残余风险),故每次按键后实测焦点。
static void refresh_fb(void) {
    sim_lvgl_lock(1000);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    sim_lvgl_unlock();
    s_man_refr_vus = s_vtime_us;   // 声明见 press 前
}
static int px_gold(int x, int y) {
    const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
    int r = p[2], g = p[1], b = p[0];
    return r > 200 && g > 150 && b < 120;
}
// 金色箭头(或页签金框)顶缘 y;限制在动作/列表区,避开页签带与金边弹窗
static int arrow_y_min(void) {
    int best = -1;
    for (int y = 66; y < 300; y++)
        for (int x = 21; x < 48; x++)
            if (px_gold(x, y)) { return y; }
    return best;
}
// 各页行首 y(内容坐标+21);返回 -1 表示该页无此行
static int row_y(ppage_t pg, int row) {
    switch (pg) {
        case PG_HOMEX: {
            int n = home_lines();
            return (row >= 0 && row < n) ? 21 + (278 - n * 24) + row * 24 + 2 : -1;
        }
        case PG_BUILDX:
            return (row >= 0 && row <= 10) ? 21 + 46 + row * 19 + 2 : -1;
        case PG_VILLAGEX: {
            static const int vy[9] = {52,75,98,121,144,167,190,213,236};
            return (row >= 0 && row <= 8) ? 21 + vy[row] + 2 : -1;
        }
        case PG_TRADEX: {
            static const int ty[9] = {46,67,88,109,130,151,172,193,215};
            return (row >= 0 && row <= 8) ? 21 + ty[row] + 2 : -1;
        }
        case PG_SETTINGSX:
            return (row >= 0 && row <= 5) ? 21 + 52 + row * 28 + 2 : -1;
        case PG_CRAFTX:
            return (row >= 0 && row <= 8) ? 21 + 52 + row * 23 + 2 : -1;
        case PG_MAP_OUTFITX: {
            static const int my[7] = {52,78,104,130,156,182,208};
            return (row >= 0 && row < 7) ? 21 + my[row] + 2 : -1;
        }
        case PG_MAP_EXPX:
        case PG_COMBATX: {
            static const int ey[5] = {168,190,212,234,256};
            int n = (pg == PG_COMBATX) ? 4 : 5;
            return (row >= 0 && row < n) ? 21 + ey[row] + 2 : -1;
        }
        case PG_RUINX: {
            static const int ry[2] = {150,174};
            return (row >= 0 && row < 2) ? 21 + ry[row] + 2 : -1;
        }
        default: return -1;
    }
}
// 实测当前焦点行(按预期页的行表匹配);-2=无箭头,-3=不匹配任何行
static int focus_measured(ppage_t pg, int lines) {
    int ay = arrow_y_min();
    if (ay < 0) return -2;
    int best = -3, bestd = 1 << 30;
    for (int r = 0; r < lines; r++) {
        int y = row_y(pg, r);
        if (y < 0) continue;
        int d = ay > y ? ay - y : y - ay;
        if (d < bestd) { bestd = d; best = r; }
    }
    return (bestd <= 8) ? best : -3;
}
// 页面结构指纹:验证当前确实在预期页(防丢键后跑错页)
static int ink_bands(int x0, int x1, int y0, int y1, int gap) {
    int n = 0, run = 0;
    for (int y = y0; y <= y1; y++) {
        int hit = 0;
        for (int x = x0; x <= x1 && !hit; x++) {
            const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
            if (p[0] + p[1] + p[2] > 110) hit = 1;
        }
        if (hit) run++;
        else if (run > 0) { if (run >= 3 || gap == 1) n++; run = 0; }
    }
    if (run > 0) n++;
    return n;
}
static int ink_any(int x0, int x1, int y0, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
            if (p[0] + p[1] + p[2] > 110) return 1;
        }
    return 0;
}
static bool page_is(ppage_t pg) {
    refresh_fb();
    int track_v = 0, track_c = 0;
    for (int y = 142; y <= 255; y++) {
        const uint8_t *p = s_fb + ((size_t)y * SIM_W + 216) * 4;
        if (p[0] + p[1] + p[2] > 140) track_v++;
    }
    for (int y = 73; y <= 255; y++) {
        const uint8_t *p = s_fb + ((size_t)y * SIM_W + 216) * 4;
        if (p[0] + p[1] + p[2] > 140) track_c++;
    }
    int tabs = 0;
    for (int y = 45; y <= 64 && !tabs; y++)
        for (int x = 21; x <= 219; x++)
            if (px_gold(x, y)) { tabs = 1; break; }
    switch (pg) {
        case PG_VILLAGEX:  return track_v > 100 && track_c < 160;
        case PG_CRAFTX:    return track_c > 160;
        case PG_HOMEX:     return tabs && ink_any(21, 213, 240, 299) &&
                                  track_v < 20 && track_c < 20;
        case PG_SETTINGSX: return tabs && !ink_any(21, 213, 240, 299);
        case PG_BUILDX: {
            int b = ink_bands(21, 213, 66, 280, 3);
            return !tabs && track_v < 20 && track_c < 20 && b >= 11;
        }
        case PG_TRADEX: {
            int b = ink_bands(21, 213, 66, 280, 3);
            return !tabs && track_v < 20 && track_c < 20 && b >= 8 && b <= 10;
        }
        case PG_MAP_EXPX:
            return !tabs && track_v < 20 && track_c < 20 &&
                   ink_any(71, 78, 64, 68) &&           // 9x9 格阵上缘(仅其左缘)
                   ink_bands(21, 213, 185, 299, 3) >= 4;
        case PG_RUINX:   // 3x3 房格上缘起于 x79(9x9 格阵起于 x71,可区分)
            return !tabs && track_v < 20 && track_c < 20 &&
                   !ink_any(72, 78, 64, 68) && ink_any(79, 157, 64, 68);
        case PG_MAP_OUTFITX: {
            int b = ink_bands(21, 213, 66, 285, 3);
            return !tabs && track_v < 20 && track_c < 20 && !ink_any(71, 169, 64, 68) &&
                   b >= 7 && b <= 9;
        }
        case PG_COMBATX: {
            int red = 0;
            for (int y = 67; y <= 77; y++)
                for (int x = 25; x <= 215; x++) {
                    const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
                    if (p[2] > 180 && p[1] < 90 && p[0] < 90) red++;
                }
            return red > 40;                             // 敌方红血条
        }
        default: return false;
    }
}
// 闭环 focus_to:按键→实测→纠正,任何丢键自愈;失败返回 false(调用方重进页)
static bool focus_to(int target, bool (*en)(int), int lines) {
    refresh_fb();
    int m = focus_measured(P, lines);
    if (m >= 0) F = m;                    // 实测覆盖模型
    for (int i = 0; i < lines * 3 + 6 && F != target; i++) {
        k_down();
        refresh_fb();
        m = focus_measured(P, lines);
        if (m >= 0) F = m; else sim_move(en, lines, 1);
    }
    return F == target;
}
// 确保在预期页(误入他页则重进),成功返回 true
static bool ensure_page(ppage_t want);   // 前向声明(enter_* 在后面定义)
static bool focus_or_reenter(int target, bool (*en)(int), int lines,
                             ppage_t want) {
    for (int i = 0; i < 3; i++) {
        if (focus_to(target, en, lines)) return true;
        printf("[%7.1fs] focus_to(%d) 卡住,复验页面\n", vt_s(), target);
        if (!ensure_page(want)) return false;
    }
    return false;
}

// ================= 页面导航(进页后结构校验,失败重试) =================
static void enter_home(void);
static void enter_village(void);
static void enter_build(void);
static void enter_trade(void);
static void enter_settings(void);
static void enter_craft(void);
// 制造窗滑块位置回读:thumb 顶 abs y = 73 + scroll×13(满 14 项时)
static void craft_sync_scroll(void) {
    refresh_fb();
    int y0 = -1;
    for (int y = 73; y <= 255; y++) {
        const uint8_t *p = s_fb + ((size_t)y * SIM_W + 216) * 4;
        if (p[2] > 140 && p[1] > 140) { y0 = y; break; }   // 滑块亮色
    }
    if (y0 < 0) { craft_scroll = 0; return; }
    int s = (y0 - 73 + 6) / 13;
    if (s < 0) s = 0;
    craft_scroll = s;
    craft_recalc();
}
static void enter_home(void) {
    for (int i = 0; i < 4; i++) {
        if (P == PG_VILLAGEX) {
            if (!focus_to(8, village_en, village_lines())) {
                k_long();          // 焦点卡住:盲打长按兜底
            } else k_ok();
        } else if (P != PG_HOMEX) {
            k_long();
        }
        P = PG_HOMEX; F = 0;
        snap();
        if (page_is(PG_HOMEX)) return;
        k_long();                  // 再兜底一次(任意页除村庄都回主页)
    }
    CHECK(false, "enter_home 四次未确认主页");
}
// 确保在预期页(误入他页则重进)
static bool ensure_page(ppage_t want) {
    if (page_is(want)) return true;
    switch (want) {
        case PG_HOMEX:     enter_home(); break;
        case PG_VILLAGEX:  enter_village(); break;
        case PG_BUILDX:    enter_build(); break;
        case PG_TRADEX:    enter_trade(); break;
        case PG_SETTINGSX: enter_settings(); break;
        case PG_CRAFTX:    enter_craft(); break;
        default: break;
    }
    return page_is(want);
}
// 导航态探针:四个页签哪个带金框/金字(导航光标或当前页)
static int nav_probe(void) {
    refresh_fb();
    int mask = 0;
    for (int t = 0; t < 4; t++) {
        int x0 = 21 + t * 50 + 2, x1 = x0 + 43;
        for (int y = 45; y <= 65; y++)
            for (int x = x0; x <= x1; x++)
                if (px_gold(x, y)) { mask |= 1 << t; y = 66; break; }
    }
    return mask;
}
static void enter_village(void) {
    for (int i = 0; i < 3; i++) {
        enter_home();
        k_up(); k_up(); k_up(); k_ok();   // 导航:小屋→≡→村庄(跳过锁定荒野)
        P = PG_VILLAGEX; F = 0;
        if (!village_en(0)) sim_move(village_en, village_lines(), 1);
        snap();
        if (page_is(PG_VILLAGEX)) return;
        printf("[%7.1fs] enter_village 尝试 %d 页面校验失败 mask=%d\n",
               vt_s(), i, nav_probe());
    }
    CHECK(false, "enter_village 三次未确认村庄页");
}
static void enter_build(void) {
    for (int i = 0; i < 3; i++) {
        enter_home();
        if (!focus_or_reenter(1, home_en, home_lines(), PG_HOMEX))
            continue;
        k_ok();
        P = PG_BUILDX; F = 0;
        if (!build_en(0)) sim_move(build_en, build_lines(), 1);
        snap();
        if (page_is(PG_BUILDX)) return;
    }
    CHECK(false, "enter_build 三次未确认建造页");
}
static void enter_trade(void) {
    for (int i = 0; i < 3; i++) {
        enter_home();
        if (!focus_or_reenter(G.building_lv[DR_BLD_WORKSHOP] > 0 ? 3 : 2,
                              home_en, home_lines(), PG_HOMEX))
            continue;
        k_ok();
        P = PG_TRADEX; F = 0;
        if (!trade_en(0)) sim_move(trade_en, trade_lines(), 1);
        snap();
        if (page_is(PG_TRADEX)) return;
    }
    CHECK(false, "enter_trade 三次未确认贸易页");
}
static void enter_settings(void) {
    for (int i = 0; i < 3; i++) {
        enter_home();
        k_up(); k_up(); k_ok();           // 导航:小屋→≡
        P = PG_SETTINGSX; F = 0;
        snap();
        if (page_is(PG_SETTINGSX)) return;
    }
    CHECK(false, "enter_settings 三次未确认设置页");
}
static void enter_craft(void) {
    CHECK(G.building_lv[DR_BLD_WORKSHOP] > 0, "工坊未建,进不了制造页");
    for (int i = 0; i < 3; i++) {
        enter_home();
        if (!focus_or_reenter(2, home_en, home_lines(), PG_HOMEX))
            continue;
        k_ok();
        P = PG_CRAFTX; F = 0;
        craft_sync_scroll();
        craft_recalc();
        if (!craft_en(0)) sim_move(craft_en, craft_lines(), 1);
        snap();
        if (page_is(PG_CRAFTX)) return;
    }
    CHECK(false, "enter_craft 三次未确认制造页");
}
// 停泊等待:确保在建造页(无周期重绘)再快进
static void park_wait(double seconds) {
    if (P != PG_BUILDX) enter_build();
    fast_wait(seconds);
    snap();
}

// ================= 动作(每个动作:导航→按键→校验→截图) =================
static void act_collect(void) {
    for (int tries = 0; tries < 4; tries++) {
        enter_village();
        CHECK(focus_or_reenter(0, village_en, village_lines(), PG_VILLAGEX),
              "采集行找不到");
        uint32_t w0 = G.res[DR_RES_WOOD];
        k_ok_saved(); snap();
        if (G.res[DR_RES_WOOD] > w0) { shot("a-collect"); return; }
        park_wait(30.0);                        // 冷却未到:再等 30s 重试
    }
    CHECK(false, "采集连续 4 次无效(冷却?)");
}
static void act_trap_once(uint32_t *t0) {
    enter_village();
    CHECK(focus_or_reenter(1, village_en, village_lines(), PG_VILLAGEX),
          "查看陷阱行找不到");
    uint32_t s0 = 0;
    for (int i = 0; i < DR_RES_KIND_COUNT; i++) s0 += G.res_total[i];
    k_ok_saved(); snap(); shot("a-trap");
    *t0 = s0;
}
static void act_trap(void) {
    uint32_t t0;
    for (int tries = 0; tries < 5; tries++) {
        act_trap_once(&t0);
        uint32_t t1 = 0;
        for (int i = 0; i < DR_RES_KIND_COUNT; i++) t1 += G.res_total[i];
        printf("[%7.1fs] trap try%d: totals %u -> %u\n", vt_s(), tries, t0, t1);
        if (t1 > t0) return;                 // 掉落入账
        park_wait(30.0);                     // 冷却边沿:再等 30s 重试
    }
    CHECK(false, "查看陷阱连续 5 次无掉落");
}
static void act_build(int bld, const char *tag) {
    enter_build();
    CHECK(focus_or_reenter(bld, build_en, build_lines(), PG_BUILDX),
          "建造 %s 找不到行", tag);
    uint8_t lv0 = G.building_lv[bld];
    k_ok_saved(); snap();
    CHECK(G.building_lv[bld] == lv0 + 1, "建造 %s 未生效", tag);
    shot(tag);
}
static void act_trade_buy(int item) {
    static const uint8_t ires[8] = {
        DR_RES_SCALES, DR_RES_TEETH, DR_RES_IRON, DR_RES_COAL,
        DR_RES_STEEL, DR_RES_BULLETS, DR_RES_MEDICINE, 0
    };
    enter_trade();
    CHECK(focus_or_reenter(item, trade_en, trade_lines(), PG_TRADEX),
          "贸易行 %d 找不到", item);
    if (item == 7) {
        uint64_t f0 = G.flags;
        k_ok_saved(); snap();
        CHECK((G.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) &&
              !(f0 & ((uint64_t)1u << DR_FLAG_COMPASS)), "买罗盘未置旗标");
        shot("a-trade-compass");
    } else {
        uint32_t r0 = G.res[ires[item]];
        k_ok_saved(); snap();
        CHECK(G.res[ires[item]] == r0 + 1, "贸易行 %d 未+1", item);
        shot("a-trade");
    }
}
// 村庄滑块顶位回读:scroll=0 时亮滑块顶在 y142
static int village_thumb_top(void) {
    refresh_fb();
    for (int y = 142; y <= 255; y++) {
        const uint8_t *p = s_fb + ((size_t)y * SIM_W + 216) * 4;
        if (p[2] > 140 && p[1] > 140) return y;
    }
    return -1;
}
static void act_job_assign(int job, int n) {
    CHECK(job >= 0 && job <= 4, "本脚本只调配窗口内职业 0..4");
    enter_village();
    // 滚窗归零(历史上误按可能滚走):焦点在顶部区按上=上翻
    for (int i = 0; i < 6 && village_thumb_top() > 146; i++) {
        focus_to(0, village_en, village_lines());
        k_up();
    }
    job_scroll = 0;
    CHECK(focus_or_reenter(3 + job, village_en, village_lines(), PG_VILLAGEX),
          "职业 job%d 行找不到", job);
    k_ok();                              // 进入人数调节(上=+1 下=−1)
    for (int i = 0; i < n || i < -n; i++) {
        if (i < n) k_up(); else k_down();
    }
    k_ok();                              // 退出调节
    snap();
    CHECK((int)G.job[job] >= n, "派工 job%d %+d 未生效(现 %u)",
          job, n, G.job[job]);
    shot("a-job");
}
static void act_craft_make(int craft, const char *tag) {
    enter_craft();
    craft_sync_scroll();
    craft_recalc();
    int v = -1;
    for (int i = 0; i < craft_total; i++)
        if (craft_vis[i] == craft) v = i;
    CHECK(v >= 0, "制造项 %s 不可见", tag);
    while (craft_scroll > v || v >= craft_scroll + craft_rows) {
        if (craft_scroll > v) {
            if (F > 1 && F < craft_rows)
                CHECK(focus_to(0, craft_en, craft_lines()), "滚窗归位失败");
            k_up(); craft_scroll--;
        } else {
            CHECK(focus_to(craft_rows - 1, craft_en, craft_lines()),
                  "滚窗到底失败");
            k_down(); craft_scroll++;
        }
        craft_sync_scroll();
        craft_recalc(); snap();
    }
    CHECK(focus_or_reenter(v - craft_scroll, craft_en, craft_lines(), PG_CRAFTX),
          "制造 %s 行找不到", tag);
    bool ok0 = dr_rules_craft_owned(&G, (uint8_t)craft);
    k_ok_saved(); snap();
    CHECK(dr_rules_craft_owned(&G, (uint8_t)craft) != ok0, "制造 %s 未生效", tag);
    shot(tag);
}

// ================= 远征段(调试口进地图页:M4 页签锁,正常游玩不可达) =================
static void sweep_n(void) {
    sim_lvgl_lock(1000);
    dr_sweep_next();
    sim_lvgl_unlock();
    frames(10);
}
// 血条读数:在 y0..y1 找颜色近似 (r,g,b) 的最长连续段,按 max_hp 折算
static int bar_hp(int y0, int y1, int r, int g, int b, int max_hp) {
    refresh_fb();
    int best = 0, run = 0;
    for (int x = 22; x <= 218; x++) {
        int hit = 0;
        for (int y = y0; y <= y1 && !hit; y++) {
            const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
            int dr = p[2] > r ? p[2] - r : r - p[2];
            int dg = p[1] > g ? p[1] - g : g - p[1];
            int db = p[0] > b ? p[0] - b : b - p[0];
            if (dr < 60 && dg < 60 && db < 60) hit = 1;
        }
        if (hit) { run++; if (run > best) best = run; } else run = 0;
    }
    return best * max_hp / 197;
}
// 战斗循环:攻击为主,HP≤5 吃干肉;页面离开战斗页即结束
static int combat_loop(void) {
    CHECK(page_is(PG_COMBATX), "战斗页指纹不符");
    shot("p-exp-combat");
    int rounds = 0;
    while (rounds < 60) {
        if (!page_is(PG_COMBATX)) break;
        P = PG_COMBATX; F = -1;
        int hp = bar_hp(99, 108, 0x4A, 0x90, 0xD9, 15);   // 我方蓝条
        int ehp = bar_hp(68, 76, 0xE4, 0x3B, 0x2F, 100);  // 敌方红条(相对)
        if (hp <= 5 && G.food > 0) {
            focus_to(4, combat_en, combat_lines());       // 吃干肉(+8)
            k_ok_saved();
            printf("[%7.1fs]   战斗:吃干肉 HP%d->?\n", vt_s(), hp);
            shot("a-eat");
        } else {
            focus_to(0, combat_en, combat_lines());       // 攻击
            k_ok();
            if ((rounds & 1) == 0)
                printf("[%7.1fs]   战斗第%d手:我HP~%d 敌条~%d%%\n",
                       vt_s(), rounds, hp, ehp);
            if ((rounds & 3) == 0) shot("a-combat");
        }
        rounds++;
    }
    snap(); shot("p-exp-combat-end");
    printf("[%7.1fs]   战斗结束(%d 手), in_wilderness=%u hp=%u\n",
           vt_s(), rounds, G.in_wilderness, G.hero_hp);
    return rounds;
}
static void expedition_phase(void) {
    printf("[%7.1fs] == 远征段(调试口:M4 页签锁) ==\n", vt_s());
    for (int i = 0; i < 5; i++) sweep_n();     // TITLE→HOME→BUILD→VILLAGE→MAP
    P = PG_MAP_OUTFITX; F = 0;
    CHECK(page_is(PG_MAP_OUTFITX), "调试口未落到整备页");
    shot("p14-expedition-outfit");
    k_ok();                                    // 进干肉调配
    for (int i = 0; i < 15; i++) k_up();       // 带 15 干肉
    k_ok(); k_ok(); k_ok();                    // 药→子弹→退出调配
    snap();
    CHECK(G.food == 15, "整备干肉未带 (%u)", G.food);
    CHECK(focus_to(5, map_outfit_en, map_outfit_lines()), "出发行找不到");
    k_ok_saved(); snap();
    CHECK(G.in_wilderness == 1, "出发未进远征态");
    shot("p15-expedition-start");
    // 南南西西→北东北东原路返回(避开 (31,32) 洞穴);战斗按遭遇插入,
    // 误入地点(兜底)则"离开地点"返回荒野
    static const int path[8] = { 1, 1, 2, 2, 3, 3, 0, 0 };
    int fights = 0;
    for (int i = 0; i < 8; i++) {
        if (!G.in_wilderness) break;          // 阵亡:物资全失回整备
        if (page_is(PG_COMBATX)) { fights += combat_loop(); continue; }
        if (page_is(PG_RUINX)) {               // 误入地点:离开
            shot("p-exp-ruin");
            P = PG_RUINX; F = -1;
            CHECK(focus_to(1, combat_en, 2), "离开地点行找不到");
            k_ok_saved(); snap();
            printf("[%7.1fs]   误入地点,已离开\n", vt_s());
        }
        P = PG_MAP_EXPX; F = -1;
        CHECK(page_is(PG_MAP_EXPX), "远征态指纹不符(第 %d 步)", i);
        CHECK(focus_to(path[i], map_exp_en, map_exp_lines()),
              "方向行 %d 找不到", path[i]);
        k_ok_saved(); snap();
        printf("[%7.1fs]   远征第%d步(方向%d) xy=(%u,%u)\n",
               vt_s(), i, path[i], G.hero_x, G.hero_y);
        shot("a-move");
        if (page_is(PG_COMBATX)) fights += combat_loop();
    }
    snap();
    if (G.in_wilderness) {
        // 仍在远征(没走到家):长按返回主页(远征态保留在档)
        printf("[%7.1fs]   远征未归位,长按返回\n", vt_s());
        k_long();
    } else {
        shot("p16-expedition-return");
    }
    P = PG_HOMEX; F = 0;
    snap();
    enter_home(); shot("p17-after-expedition");
    printf("[%7.1fs] == 远征段结束:战斗 %d 手 ==\n", vt_s(), fights);
}

// ================= 主线剧本 =================
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s_out = argc > 1 ? argv[1] : "";
    s_t0_us = s_vtime_us;

    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    darkroom_app_enter();
    frames(60);
    P = PG_TITLEX;
    snap();
    CHECK(G.res[DR_RES_WOOD] == 15 && G.fire_lv == 0, "新档初始态异常");
    shot("p0-title");

    // ---- P1 开局:点火 → 建造者 → 森林 → 帮忙 ----
    k_ok(); P = PG_HOMEX; F = 0; snap();
    shot("p1-home-newgame");
    enter_home();
    CHECK(focus_or_reenter(0, home_en, home_lines(), PG_HOMEX), "点火行找不到");
    k_ok_saved(); snap();
    CHECK(G.fire_lv == DR_FIRE_BURNING, "点火未直达旺盛 (%u)", G.fire_lv);
    CHECK(G.res[DR_RES_WOOD] == 10, "点火未扣 5 木 (%u)", G.res[DR_RES_WOOD]);
    shot("p2-fire-lit");
    for (int i = 0; i < 40 && !(G.flags & ((uint64_t)1u << DR_FLAG_FOREST)); i++)
        park_wait(10.0);
    CHECK(G.flags & ((uint64_t)1u << DR_FLAG_FOREST), "森林未解锁");
    CHECK(G.res[DR_RES_WOOD] == 4, "森林解锁应把木置 4 (现 %u)", G.res[DR_RES_WOOD]);
    enter_home(); shot("p3-forest-open");
    for (int i = 0; i < 60 && G.builder_lv < DR_BUILDER_SLEEP; i++)
        park_wait(10.0);
    CHECK(G.builder_lv == DR_BUILDER_SLEEP, "建造者未入沉睡 (%u)", G.builder_lv);
    enter_village(); shot("p4-village-first");
    enter_home(); snap();               // 回房触发 visit:沉睡→帮忙
    CHECK(G.builder_lv == DR_BUILDER_HELP, "回房未见帮忙 (%u)", G.builder_lv);
    CHECK(G.temp_lv >= DR_TEMP_MILD, "温度未到微温 (%u)", G.temp_lv);
    shot("p5-builder-help");

    // ---- P2 基建:陷阱 → 板车 → 小屋 ----
    for (int i = 0; i < 40 && G.building_lv[DR_BLD_TRAP] == 0; i++) {
        if (G.res[DR_RES_WOOD] >= 10) act_build(DR_BLD_TRAP, "陷阱Lv1");
        else park_wait(15.0);
    }
    for (int i = 0; i < 40 && G.building_lv[DR_BLD_CART] == 0; i++) {
        if (G.res[DR_RES_WOOD] >= 30) act_build(DR_BLD_CART, "板车");
        else { act_collect(); park_wait(62.0); }
    }
    CHECK(G.building_lv[DR_BLD_CART] == 1, "板车未建");
    for (int i = 0; i < 30 && G.building_lv[DR_BLD_HUT] == 0; i++) {
        if (G.res[DR_RES_WOOD] >= 100) act_build(DR_BLD_HUT, "小屋Lv1");
        else { act_collect(); park_wait(62.0); }
    }
    CHECK(G.building_lv[DR_BLD_HUT] == 1, "小屋Lv1 未建");
    shot("p6-hut1-pop-growing");

    // ---- P3 陷阱升到 Lv8,小屋升到 Lv7(人口 28) ----
    for (int lv = 2; lv <= 8; lv++)
        for (int i = 0; i < 60 && G.building_lv[DR_BLD_TRAP] < lv; i++) {
            if (G.res[DR_RES_WOOD] >= 10 + 10u * G.building_lv[DR_BLD_TRAP] + 40)
                act_build(DR_BLD_TRAP, "陷阱升级");
            else park_wait(60.0);
        }
    for (int hut = 2; hut <= 7; hut++)
        for (int i = 0; i < 120 && G.building_lv[DR_BLD_HUT] < hut; i++) {
            if (G.population >= dr_rules_pop_cap(&G) - 2 &&
                G.res[DR_RES_WOOD] >= 100 + 50u * G.building_lv[DR_BLD_HUT])
                act_build(DR_BLD_HUT, "小屋升级");
            else park_wait(60.0);
        }
    for (int i = 0; i < 90 && G.population < 26; i++)
        park_wait(60.0);
    snap();
    CHECK(G.building_lv[DR_BLD_HUT] == 7 && G.building_lv[DR_BLD_TRAP] == 8,
          "小屋/陷阱规模不符 (%u/%u)",
          G.building_lv[DR_BLD_HUT], G.building_lv[DR_BLD_TRAP]);
    CHECK(G.population >= 26, "人口不足 (%u)", G.population);
    enter_village(); shot("p7-village-pop");

    // ---- P4 陷阱收网(毛/肉) → 猎屋 → 猎人经济(攒毛) ----
    for (int i = 0; i < 120 && G.building_lv[DR_BLD_LODGE] == 0; i++) {
        if (G.res[DR_RES_FUR] >= 10 && G.res[DR_RES_MEAT] >= 5 &&
            G.res[DR_RES_WOOD] >= 200) act_build(DR_BLD_LODGE, "猎屋");
        else { act_trap(); park_wait(92.0); }
    }
    CHECK(G.building_lv[DR_BLD_LODGE] == 1, "猎屋未建");
    act_job_assign(DR_JOB_HUNTER, 18);
    act_job_assign(DR_JOB_TRAPPER, 2);
    enter_home(); shot("p8-hunters-on");
    for (int i = 0; i < 220 && G.res[DR_RES_FUR] < 4800; i++)
        park_wait(60.0);
    CHECK(G.res[DR_RES_FUR] >= 4800, "猎人攒毛不足 (%u)", G.res[DR_RES_FUR]);

    // ---- P5 贸易站/革坊/熏房 + 派工 → 攒革 ----
    act_build(DR_BLD_TRADE_POST, "贸易站");
    act_build(DR_BLD_TANNERY, "制革坊");
    act_build(DR_BLD_SMOKEHOUSE, "熏肉房");
    act_job_assign(DR_JOB_TANNER, 4);
    act_job_assign(DR_JOB_CHARCUTIER, 1);
    enter_village(); shot("p9-jobs-full");
    for (int i = 0; i < 240 && G.res[DR_RES_LEATHER] < 760; i++)
        park_wait(60.0);
    CHECK(G.res[DR_RES_LEATHER] >= 760, "制革产出不足 (%u)", G.res[DR_RES_LEATHER]);

    // ---- P6 陷阱循环:囤鳞/牙/布(贸易与制造原料) ----
    for (int i = 0; i < 160 &&
         (G.res[DR_RES_SCALES] < 220 || G.res[DR_RES_TEETH] < 150 ||
          G.res[DR_RES_CLOTH] < 1); i++) {
        act_trap();
        park_wait(92.0);
    }
    CHECK(G.res[DR_RES_CLOTH] >= 1, "陷阱未掉过布(火把没材料)");
    CHECK(G.res[DR_RES_SCALES] >= 220 && G.res[DR_RES_TEETH] >= 150,
          "鳞/牙不足 (鳞%u 牙%u)", G.res[DR_RES_SCALES], G.res[DR_RES_TEETH]);
    enter_village(); shot("p10-trap-loot");
    act_job_assign(DR_JOB_TANNER, -4);   // 制革匠清零:让毛皮回攒(贸易备料)
    enter_village(); shot("p11-tanner-off");
    for (int i = 0; i < 200 && G.res[DR_RES_FUR] < 1700; i++)
        park_wait(60.0);
    CHECK(G.res[DR_RES_FUR] >= 1700, "贸易备毛不足 (%u)", G.res[DR_RES_FUR]);

    // ---- P7 贸易全部 8 行 ----
    for (int item = 0; item < 8; item++) act_trade_buy(item);
    snap();
    CHECK(G.flags & ((uint64_t)1u << DR_FLAG_COMPASS), "罗盘未购入");
    CHECK(G.res[DR_RES_IRON] >= 1 && G.res[DR_RES_COAL] >= 1 &&
          G.res[DR_RES_STEEL] >= 1 && G.res[DR_RES_BULLETS] >= 1 &&
          G.res[DR_RES_MEDICINE] >= 1, "贸易采购未全部落账");
    enter_trade(); shot("p11-trade-all-rows");

    // ---- P8 工坊 + 制造可达项 ----
    for (int i = 0; i < 120 && !(G.res[DR_RES_WOOD] >= 800 &&
           G.res[DR_RES_LEATHER] >= 100 && G.res[DR_RES_SCALES] >= 10); i++)
        park_wait(60.0);
    act_build(DR_BLD_WORKSHOP, "工坊");
    act_craft_make(DR_CRAFT_TORCH, "火把");
    act_craft_make(DR_CRAFT_BONE_SPEAR, "骨矛");
    act_craft_make(DR_CRAFT_WATERSKIN, "水袋");
    act_craft_make(DR_CRAFT_RUCKSACK, "背囊");
    act_craft_make(DR_CRAFT_L_ARMOUR, "皮甲");
    enter_craft(); shot("p12-crafts-done");
    snap();
    CHECK(G.weapon_lv == 1 && G.armor_lv == 1, "武器/护甲阶未生效 (%u/%u)",
          G.weapon_lv, G.armor_lv);
    CHECK((G.flags & ((uint64_t)1u << DR_FLAG_TORCH)) &&
          (G.flags & ((uint64_t)1u << DR_FLAG_WATERSKIN)) &&
          (G.flags & ((uint64_t)1u << DR_FLAG_RUCKSACK)), "制造旗标未置位");

    // ---- P9 终态断言 + 设置 + 重开确认 ----
    snap();
    uint8_t want_bld[] = { DR_BLD_CART, DR_BLD_TRAP, DR_BLD_HUT, DR_BLD_LODGE,
                           DR_BLD_TRADE_POST, DR_BLD_TANNERY, DR_BLD_SMOKEHOUSE,
                           DR_BLD_WORKSHOP };
    for (int i = 0; i < 8; i++)
        CHECK(G.building_lv[want_bld[i]] >= 1, "建筑 %d 未建", want_bld[i]);
    CHECK(G.job[DR_JOB_HUNTER] == 18 && G.job[DR_JOB_TRAPPER] == 2 &&
          G.job[DR_JOB_CHARCUTIER] == 1 && G.job[DR_JOB_TANNER] == 0,
          "职业编制不符 (猎%u 捕%u 革%u 熏%u)", G.job[DR_JOB_HUNTER],
          G.job[DR_JOB_TRAPPER], G.job[DR_JOB_TANNER],
          G.job[DR_JOB_CHARCUTIER]);
    CHECK(G.fire_lv >= 2 && G.temp_lv >= 2, "火/温度失守 (%u/%u)",
          G.fire_lv, G.temp_lv);
    enter_home(); shot("p13-endgame-home");

    expedition_phase();      // 真实远征+战斗(M4 锁门,调试口进地图页)

    enter_settings();
    shot("p14-settings");
    focus_to(1, settings_en, settings_lines());
    k_ok(); P = PG_TITLEX; F = 0; snap();   // 确认弹窗(借用 PG 枚举占位)
    shot("p15-reset-confirm");
    k_ok_saved(); snap();
    CHECK(G.res[DR_RES_WOOD] == 15 && G.fire_lv == 0 && G.population == 0,
          "重开本局未回到新档");
    shot("p16-fresh-newgame");

    printf("\n== PLAYTHROUGH OK ==\n");
    printf("virtual %.1f min, shots %d, final: hut=7 trap=8 pop=%u wood=%u\n",
           vt_s() / 60.0, s_shot_n, G.population, G.res[DR_RES_WOOD]);
    sim_lvgl_lock(1000);
    dr_debug_dump();
    sim_lvgl_unlock();
    return 0;
}
