// main/dr_port.c —— NVS 存档适配 + Unix 时间源。
#include <string.h>
#include <sys/time.h>

#include "dr_config.h"
#include "dr_port.h"
#include "dr_util.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dr_port";

int dr_port_storage_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重试:%s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return (int)err;
}

uint32_t dr_port_now_ts(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 0) return 0;
    // 2038 之前的时间戳都装得下;RTC 未校准时是 1970 附近,由回拨兜底处理
    return (uint32_t)tv.tv_sec;
}

int dr_port_save(const dr_game_t *g) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("darkroom", NVS_READWRITE, &h);
    if (err != ESP_OK) return (int)err;

    dr_save_image_t img;
    dr_state_pack(g, &img);
    err = nvs_set_blob(h, DR_SAVE_NVS_KEY, &img, sizeof(img));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "存档失败:%s", esp_err_to_name(err));
    return (int)err;
}

int dr_port_load(dr_game_t *g, bool *out_loaded, uint32_t *out_offline_ticks) {
    *out_loaded = false;
    *out_offline_ticks = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open("darkroom", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;  // 空档
    if (err != ESP_OK) return (int)err;

    dr_save_image_t img;
    size_t len = sizeof(img);
    err = nvs_get_blob(h, DR_SAVE_NVS_KEY, &img, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK || len != sizeof(img)) {
        ESP_LOGE(TAG, "读档失败(len=%u):%s", (unsigned)len, esp_err_to_name(err));
        return 0;  // 坏档:按空档处理,游戏可开新局
    }

    dr_game_t body;
    uint16_t ver = 0;
    if (!dr_state_unpack(&img, &body, &ver)) {
        ESP_LOGW(TAG, "存档校验失败(CRC/版本),开新档");
        return 0;
    }
    if (!dr_state_migrate(&body, ver)) {
        ESP_LOGW(TAG, "存档版本 %u 无法迁移,开新档", ver);
        return 0;
    }
    *out_loaded = true;
    *out_offline_ticks = dr_offline_ticks(&body, dr_port_now_ts());
    *g = body;
    ESP_LOGI(TAG, "读档成功 v%u,离线结算 %u tick", ver, *out_offline_ticks);
    return 0;
}
