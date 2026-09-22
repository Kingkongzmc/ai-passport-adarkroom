// tools/sim/sim_dark.c —— 《小黑屋》主机模拟器(横屏 320×240)。
// LVGL 全源码 + 垫片运行真实游戏代码;虚拟时间每帧 +30ms,
// 按键脚本注入(动作绑 PRESS,本游戏只认按下沿),帧缓冲导出 BMP。
//
// 用法: sim_dark.exe [按键脚本] [帧数] [输出BMP]
//   按键脚本 "ok@5,down@10,ok@15"(按钮@帧号;均为 PRESS 事件)
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

// ---------------------------------------------------------------------------
// 虚拟时间:主循环每帧推进 30ms;esp_timer_get_time 全线程可见。
// ---------------------------------------------------------------------------
static volatile int64_t s_vtime_us = 1000000;  // 从 1s 起,避开开机防抖

int64_t esp_timer_get_time(void) { return s_vtime_us; }

// ---------------------------------------------------------------------------
// FreeRTOS 队列/任务垫片:key_task 跑在 pthread 里
// ---------------------------------------------------------------------------
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
    if (s_thread_cnt >= 4) return pdPASS == 0;  // pdFAIL
    pthread_create(&s_threads[s_thread_cnt], NULL, (void *(*)(void *))fn, arg);
    s_thread_cnt++;
    return pdPASS;
}
void vTaskDelete(void *t) { (void)t; /* 模拟器进程随 main 退出 */ }
void vTaskDelay(TickType_t ms) { usleep((useconds_t)ms * 1000); }

// ---------------------------------------------------------------------------
// dr_port 桩:存档留在内存,时间用虚拟时钟
// ---------------------------------------------------------------------------
static dr_save_image_t s_save_img;
static bool s_save_loaded;
int dr_port_storage_init(void) { return 0; }
uint32_t dr_port_now_ts(void) { return (uint32_t)(s_vtime_us / 1000000); }
int dr_port_save(const dr_game_t *g) {
    dr_state_pack(g, &s_save_img);
    s_save_loaded = true;
    return 0;
}
int dr_port_load(dr_game_t *g, bool *out_loaded, uint32_t *out_ticks) {
    *out_loaded = false; *out_ticks = 0;
    if (!s_save_loaded) return 0;         // 每次运行都开新档
    // 读回上次内存档但离线按 0 计,便于脚本重放
    uint16_t ver;
    if (dr_state_unpack(&s_save_img, g, &ver)) *out_loaded = true;
    return 0;
}

// ---------------------------------------------------------------------------
// LVGL 全局锁:按键线程与主循环互斥(真机由 esp_lvgl_port 提供)
// ---------------------------------------------------------------------------
static pthread_mutex_t s_lvgl_mtx = PTHREAD_MUTEX_INITIALIZER;
bool sim_lvgl_lock(int timeout_ms) {
    (void)timeout_ms;
    return pthread_mutex_lock(&s_lvgl_mtx) == 0;
}
void sim_lvgl_unlock(void) { pthread_mutex_unlock(&s_lvgl_mtx); }

// ---------------------------------------------------------------------------
// 显示:内存帧缓冲(XRGB8888)+ BMP 导出
// ---------------------------------------------------------------------------
static uint8_t s_fb[SIM_W * SIM_H * 4];
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}
static void write_bmp(const char *path);
void sim_write_bmp_now(const char *path) { write_bmp(path); }
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

// ---------------------------------------------------------------------------
// 按键脚本 + 主循环
// ---------------------------------------------------------------------------
typedef struct { bsp_btn_t btn; int frame; } sim_key_t;
static sim_key_t s_keys[128]; static int s_key_count;

static void parse_keys(char *spec) {
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        char name[16] = {0}; int frame = 0;
        if (sscanf(tok, "%15[^@]@%d", name, &frame) != 2) continue;
        bsp_btn_t btn = strcmp(name, "up") == 0 ? BSP_BTN_UP :
                        strcmp(name, "down") == 0 ? BSP_BTN_DOWN : BSP_BTN_OK;
        if (s_key_count < 128) {
            s_keys[s_key_count].btn = btn;
            s_keys[s_key_count].frame = frame;
            s_key_count++;
        }
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    char *keyspec = argc > 1 ? argv[1] : NULL;
    int frames = argc > 2 ? atoi(argv[2]) : 100;
    const char *bmp = argc > 3 ? argv[3] : "sim_dark.bmp";
    if (keyspec) parse_keys(keyspec);

    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    darkroom_app_enter();

    for (int f = 0; f < frames; f++) {
        for (int k = 0; k < s_key_count; k++)
            if (s_keys[k].frame == f)
                darkroom_app_key(s_keys[k].btn, BSP_BTN_PRESS);
        s_vtime_us += 30000;      // 每帧 30ms
        lv_tick_inc(30);
        if (getenv("SIM_VERBOSE")) { fprintf(stderr, "frame %d pre\n", f); fflush(stderr); }
        sim_lvgl_lock(1000);
        // 每 50 帧(1.5s)切一页:大于 1s 渲染心跳,保证每页至少被渲染一次
        if (getenv("SIM_PAGE_SWEEP") && f % 50 == 20) dr_sweep_next();
        if (getenv("SIM_FORCE")) lv_obj_invalidate(lv_screen_active());
        lv_timer_handler();
        if (getenv("SIM_VERBOSE")) { fprintf(stderr, "frame %d post\n", f); fflush(stderr); }
        sim_lvgl_unlock();
        usleep(1000);             // 给 key_task/save 线程让出 CPU
        // 页面轮播:每页保持期末落一张 BMP(SIM_PAGE_SWEEP 模式)
        if (getenv("SIM_PAGE_SWEEP") && f % 50 == 45) {
            char p[32];
            snprintf(p, sizeof(p), "sim_f%04d.bmp", f);
            extern void sim_write_bmp_now(const char *path);
            sim_write_bmp_now(p);
        }
        // SIM_CAPTURE=前缀:每帧落盘一张 BMP(诊断页面切换时序用)
        if (getenv("SIM_CAPTURE")) {
            char p[64];
            snprintf(p, sizeof(p), "%s%04d.bmp", getenv("SIM_CAPTURE"), f);
            extern void sim_write_bmp_now(const char *path);
            sim_write_bmp_now(p);
        }
    }
    sim_lvgl_lock(1000);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);           // 强制同步重绘,排除刷新时序问题
    sim_lvgl_unlock();
    write_bmp(bmp);
    sim_lvgl_lock(1000);
    dr_debug_dump();
    sim_lvgl_unlock();
    printf("sim_dark done: %d frames -> %s\n", frames, bmp);
    return 0;
}
