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

int dr_port_load(dr_game_t *g, bool *out_loaded) {
    *out_loaded = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open("darkroom", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;  // 空档
    if (err != ESP_OK) return (int)err;

    dr_save_image_t img;
    memset(&img, 0, sizeof(img));
    size_t len = sizeof(img);
    err = nvs_get_blob(h, DR_SAVE_NVS_KEY, &img, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读档失败:%s", esp_err_to_name(err));
        return 0;  // 坏档:按空档处理,游戏可开新局
    }
    // 长度以实际存档为准:旧版本固件写的镜像更短,拒绝"长度不符=坏档"
    // (那会把升级用户的存档误杀)。dr_state_load 内部按内容长度验 CRC
    // 并逐版本迁移,v1 存档升级到 v2 后原班数据继续用。
    if (len < sizeof(dr_save_hdr_t) || len > sizeof(img)) {
        ESP_LOGE(TAG, "存档长度异常(%u),开新档", (unsigned)len);
        return 0;
    }

    dr_game_t body;
    if (!dr_state_load(&img, len, &body)) {
        ESP_LOGW(TAG, "存档校验失败(CRC/版本),开新档");
        return 0;
    }
    // 纯读取:saved_at_ts 原样带出,离线间隔由 dr_rules_offline_settle
    // 消费(此前这里顺手调 dr_offline_ticks 把锚点推到当下,结算看到
    // Δt=0 直接早退——真机离线收益从未生效,v1 带病)。
    *out_loaded = true;
    *g = body;
    ESP_LOGI(TAG, "读档成功(len=%u,当前结构 v%u)",
             (unsigned)len, (unsigned)DR_SAVE_VERSION);
    return 0;
}
