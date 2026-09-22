// tools/sim/sim_main.c —— 恶魔轮盘主机模拟器。
// 用 LVGL 全源码 + FreeRTOS/BSP 垫片在主机上运行真实游戏代码,
// 帧缓冲导出 BMP"看"画面,按键按脚本注入,用于不烧录设备的功能调试。
//
// 用法: sim.exe [按键脚本] [帧数] [输出BMP] [SIM_TRACE=1 逐帧打印]
//   按键脚本形如 "ok@10,up@20,okl@30"(按钮@帧号,长按加 l 后缀)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "roulette_app.h"
#include "roulette_sfx.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#define SIM_W 240
#define SIM_H 320

// ---------------------------------------------------------------------------
// FreeRTOS 队列垫片
// ---------------------------------------------------------------------------
struct sim_queue {
    int item_size;
    int head;
    int count;
    int capacity;
    uint8_t *storage;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct sim_queue *q = calloc(1, sizeof(*q));
    if (!q) abort();
    q->item_size = (int)item_size;
    q->capacity = (int)length;
    q->storage = calloc(length, item_size);
    if (!q->storage) abort();
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    (void)wait;
    if (!q || q->count >= q->capacity) return pdFALSE;
    int tail = (q->head + q->count) % q->capacity;
    memcpy(q->storage + tail * q->item_size, item, (size_t)q->item_size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) {
    (void)wait;
    if (!q || q->count == 0) return pdFALSE;
    memcpy(out, q->storage + q->head * q->item_size, (size_t)q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t q) {
    if (!q) return;
    free(q->storage);
    free(q);
}

// ---------------------------------------------------------------------------
// esp_timer 垫片:SIM_SEED 环境变量可固定随机种子,复现特定对局。
// ---------------------------------------------------------------------------
int64_t esp_timer_get_time(void) {
    const char *seed = getenv("SIM_SEED");
    if (seed) return (int64_t)atoll(seed) * 1000;
    return (int64_t)clock() * 1000;
}

// ---------------------------------------------------------------------------
// 音效桩:声音按需求最后接入,模拟器不发声
// ---------------------------------------------------------------------------
esp_err_t roulette_sfx_start(void) { return 0; }
esp_err_t roulette_sfx_stop(void) { return 0; }
void roulette_sfx_play(roulette_sfx_t id) { (void)id; }

// ---------------------------------------------------------------------------
// 显示:内存帧缓冲(XRGB8888) + BMP 导出
// ---------------------------------------------------------------------------
static uint8_t s_fb[SIM_W * SIM_H * 4];

static void flush_cb(lv_display_t *disp, const lv_area_t *area,
                     uint8_t *px_map) {
    (void)disp;
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static void write_bmp(const char *path) {
    uint32_t row_size = (uint32_t)SIM_W * 3;
    uint32_t data_size = row_size * SIM_H;
    uint8_t header[54] = {0};
    uint32_t file_size = 54 + data_size;
    uint32_t offset = 54;
    uint32_t dib = 40;
    int32_t w = SIM_W;
    int32_t h = SIM_H;
    uint16_t planes = 1;
    uint16_t bpp = 24;
    header[0] = 'B';
    header[1] = 'M';
    memcpy(header + 2, &file_size, 4);
    memcpy(header + 10, &offset, 4);
    memcpy(header + 14, &dib, 4);
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &data_size, 4);

    uint8_t *rows = malloc(data_size);
    if (!rows) abort();
    for (int y = 0; y < SIM_H; y++) {
        uint8_t *dst = rows + (uint32_t)(SIM_H - 1 - y) * row_size;
        for (int x = 0; x < SIM_W; x++) {
            // LVGL XRGB8888 小端内存序: B,G,R,X。
            const uint8_t *p = s_fb + ((uint32_t)y * SIM_W + x) * 4;
            dst[x * 3 + 0] = p[0];
            dst[x * 3 + 1] = p[1];
            dst[x * 3 + 2] = p[2];
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        free(rows);
        exit(1);
    }
    fwrite(header, 1, sizeof(header), f);
    fwrite(rows, 1, data_size, f);
    fclose(f);
    free(rows);
}

// ---------------------------------------------------------------------------
// 按键脚本解析与主循环
// ---------------------------------------------------------------------------
typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    int frame;
} sim_key_t;

static sim_key_t s_keys[64];
static int s_key_count;

static void parse_keys(char *spec) {
    static const struct { const char *name; bsp_btn_t btn; bsp_btn_ev_t ev; } table[] = {
        {"up", BSP_BTN_UP, BSP_BTN_CLICK},
        {"down", BSP_BTN_DOWN, BSP_BTN_CLICK},
        {"ok", BSP_BTN_OK, BSP_BTN_CLICK},
        {"upl", BSP_BTN_UP, BSP_BTN_LONG},
        {"downl", BSP_BTN_DOWN, BSP_BTN_LONG},
        {"okl", BSP_BTN_OK, BSP_BTN_LONG},
    };
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        char name[16] = {0};
        int frame = 0;
        if (sscanf(tok, "%15[^@]@%d", name, &frame) != 2) continue;
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            if (strcmp(name, table[i].name) == 0 && s_key_count < 64) {
                s_keys[s_key_count].btn = table[i].btn;
                s_keys[s_key_count].ev = table[i].ev;
                s_keys[s_key_count].frame = frame;
                s_key_count++;
            }
        }
    }
}

int main(int argc, char **argv) {
    char *keyspec = argc > 1 ? argv[1] : NULL;
    int frames = argc > 2 ? atoi(argv[2]) : 100;
    const char *bmp = argc > 3 ? argv[3] : "sim_frame.bmp";
    int trace = getenv("SIM_TRACE") != NULL;
    if (keyspec) parse_keys(keyspec);

    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    roulette_app_enter();
    roulette_app_start();

    printf("sim: enter done, running %d frames\n", frames);
    int dump_every = getenv("SIM_DUMP_EVERY") ? atoi(getenv("SIM_DUMP_EVERY")) : 0;
    for (int f = 0; f < frames; f++) {
        for (int k = 0; k < s_key_count; k++) {
            if (s_keys[k].frame == f) {
                roulette_app_key(s_keys[k].btn, s_keys[k].ev);
            }
        }
        lv_tick_inc(30);
        uint32_t next = lv_timer_handler();
        (void)next;
        if (trace) printf("frame %d\n", f);
        if (dump_every && f % dump_every == 0) {
            char path[64];
            snprintf(path, sizeof(path), "sim_f%04d.bmp", f);
            write_bmp(path);
        }
    }
    write_bmp(bmp);
    printf("sim done: %d frames -> %s\n", frames, bmp);
    return 0;
}
