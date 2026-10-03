// sim_pace.c —— 验收专用调试副本(基于 tools/sim/sim_dark.c 的垫片,项目源码未动)。
// 与 sim_dark 的差异:按键不再走紧凑脚本,而是每键后推进 14 帧(420ms 虚拟时间),
// 确定性等待 key_task 消费+渲染,规避渲染静默期竞态;页面跳转直接调 dr_sweep_next。
// 用法: sim_pace.exe <输出目录前缀>(BMP 写到前缀路径;存档读 cwd 的 sim_dr_save.bin)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "darkroom_app.h"
#include "dr_port.h"
#include "dr_state.h"
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
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) {
    struct sim_queue *s = (struct sim_queue *)q;
    if (s->count == 0) { usleep(1000); return pdFALSE; }
    memcpy(out, s->storage + s->head * s->item_size, (size_t)s->item_size);
    s->head = (s->head + 1) % s->capacity;
    s->count--;
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
static bool s_save_persist;
int dr_port_storage_init(void) { return 0; }
uint32_t dr_port_now_ts(void) { return (uint32_t)(s_vtime_us / 1000000); }
int dr_port_save(const dr_game_t *g) {
    dr_state_pack(g, &s_save_img);
    if (!s_save_persist) return 0;
    FILE *f = fopen("sim_dr_save.bin", "wb");
    if (!f) return -1;
    fwrite(&s_save_img, 1, sizeof(s_save_img), f);
    fclose(f);
    return 0;
}
int dr_port_load(dr_game_t *g, bool *out_loaded) {
    *out_loaded = false;
    if (s_save_persist) {
        FILE *f = fopen("sim_dr_save.bin", "rb");
        if (!f) return 0;
        uint8_t buf[sizeof(dr_save_image_t)];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (n >= sizeof(dr_save_hdr_t) && dr_state_load(buf, n, g))
            *out_loaded = true;
        return 0;
    }
    uint16_t ver;
    if (dr_state_unpack(&s_save_img, g, &ver)) *out_loaded = true;
    return 0;
}
int dr_port_log_save(const void *blob, size_t len) {
    if (!s_save_persist) return 0;
    FILE *f = fopen("sim_dr_log.bin", "wb");
    if (!f) return -1;
    fwrite(blob, 1, len, f);
    fclose(f);
    return 0;
}
int dr_port_log_load(void *buf, size_t cap, size_t *out_len) {
    *out_len = 0;
    if (!s_save_persist) return 0;
    FILE *f = fopen("sim_dr_log.bin", "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    if (n > 0) *out_len = n;
    return 0;
}

static pthread_mutex_t s_lvgl_mtx = PTHREAD_MUTEX_INITIALIZER;
bool sim_lvgl_lock(int timeout_ms) {
    (void)timeout_ms;
    return pthread_mutex_lock(&s_lvgl_mtx) == 0;
}
void sim_lvgl_unlock(void) { pthread_mutex_unlock(&s_lvgl_mtx); }

static uint8_t s_fb[SIM_W * SIM_H * 4];
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}
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

static const char *s_prefix = "";

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
static void shot(const char *name) {
    char p[128];
    snprintf(p, sizeof(p), "%s%s.bmp", s_prefix, name);
    sim_lvgl_lock(1000);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    sim_lvgl_unlock();
    write_bmp(p);
    printf("shot %s @ vtime %lldms\n", p, (long long)(s_vtime_us / 1000));
}
static void key(bsp_btn_t b, bsp_btn_ev_t ev) {
    // 确定性消键三段式:
    // ① 刷 3 帧——让挂起的 tick 渲染(规则事件 dirty)在本段内完成;
    // ② 空转 3 帧——无 dirty 不再渲染,把 last_render 与当下拉开 ≥90ms 虚拟间隔;
    // ③ 发键后停转主循环 150ms 真实时间——key_task 在无任何渲染竞争时消费,
    //    此时 now_ms-last_render_ms ≥90ms > 60ms 静默门,必不被吞。
    frames(3);
    frames(3);
    darkroom_app_key(b, ev);
    usleep(150000);
    frames(6);
}
static void sweep_n(void) {
    sim_lvgl_lock(1000);
    dr_sweep_next();
    sim_lvgl_unlock();
    frames(10);   // 300ms:渲染落地后再空转,拉开与下一键的虚拟间隔
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s_prefix = argc > 1 ? argv[1] : "";
    s_save_persist = getenv("SIM_PERSIST") != NULL;
    char *start_ts = getenv("SIM_START_TS");
    if (start_ts && atoll(start_ts) > 0)
        s_vtime_us = atoll(start_ts) * 1000000;

    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    darkroom_app_enter();
    frames(60);                    // 过开机 1.5s 防抖
    shot("p00_title");

    key(BSP_BTN_OK, BSP_BTN_PRESS);            // → 主页
    shot("p01_home");
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);          // focus=建造
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // → 建造页
    shot("p02_build");
    key(BSP_BTN_OK, BSP_BTN_LONG);             // 长按返回 → 主页
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);          // focus=贸易
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // → 贸易页
    shot("p03_trade");
    key(BSP_BTN_OK, BSP_BTN_LONG);             // → 主页
    key(BSP_BTN_UP, BSP_BTN_PRESS);            // 进导航栏(小屋)
    key(BSP_BTN_UP, BSP_BTN_PRESS);            // ≡
    key(BSP_BTN_UP, BSP_BTN_PRESS);            // 村庄(跳过锁定荒野)
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // → 村庄页
    shot("p04_village");
    key(BSP_BTN_UP, BSP_BTN_PRESS);            // focus 0→8(返回,回环)
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // → 主页

    // 轮播跳到地图页(整备态):TITLE→HOME→BUILD→VILLAGE→MAP
    sweep_n(); sweep_n(); sweep_n(); sweep_n(); sweep_n();
    shot("p05_map_outfit");
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);          // focus=出发(武器/护甲只读跳过)
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // 出发 → 远征态
    shot("p06_map_exp");
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);          // focus=南
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // 南
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // 南
    key(BSP_BTN_DOWN, BSP_BTN_PRESS);          // focus=西
    key(BSP_BTN_OK, BSP_BTN_PRESS);            // 西 → 老屋 → 地点页
    shot("p07_ruin_house");
    sweep_n(); sweep_n(); sweep_n();           // RUIN→COMBAT→TRADE→SETTINGS
    shot("p08_settings");
    sweep_n();                                  // CONFIRM(事件空表跳过)
    shot("p09_confirm");

    sim_lvgl_lock(1000);
    dr_debug_dump();
    sim_lvgl_unlock();
    printf("sim_pace done\n");
    return 0;
}
