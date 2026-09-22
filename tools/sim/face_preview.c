// tools/sim/face_preview.c —— 荷官表情预览:四种表情各导出一张 BMP,
// 供不烧录设备时调整五官。用法: face_preview.exe [输出目录前缀]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "roulette_face.h"

#define SIM_W 240
#define SIM_H 320

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

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        exit(1);
    }
    fwrite(header, 1, sizeof(header), f);
    for (int y = SIM_H - 1; y >= 0; y--) {
        for (int x = 0; x < SIM_W; x++) {
            const uint8_t *p = s_fb + ((uint32_t)y * SIM_W + x) * 4;
            uint8_t bgr[3] = {p[0], p[1], p[2]};
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *prefix = argc > 1 ? argv[1] : "face";
    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    roulette_face_create(scr, 72, 40);

    const roulette_face_t exprs[4] = {
        ROULETTE_FACE_CALM, ROULETTE_FACE_SMUG,
        ROULETTE_FACE_HURT, ROULETTE_FACE_LAUGH,
    };
    const char *names[4] = {"calm", "smug", "hurt", "laugh"};
    for (int i = 0; i < 4; i++) {
        roulette_face_redraw(exprs[i]);
        lv_obj_invalidate(lv_screen_active());
        for (int r = 0; r < 3; r++) {
            lv_tick_inc(30);
            lv_timer_handler();
        }
        char path[64];
        snprintf(path, sizeof(path), "%s_%s.bmp", prefix, names[i]);
        write_bmp(path);
        printf("wrote %s\n", path);
    }
    return 0;
}
