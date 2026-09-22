// main/main.c —— 《小黑屋》独立应用:BSP 初始化 + 按键分发 + 开机直接进入游戏。
// 形态参照 roulette 单应用仓:不加载演示菜单,引擎纯逻辑在 game_darkroom/。
//
// 按键语义(全局,详见 docs/CONTROLS.zh_CN.md):
//   上/下 单击   移动焦点(循环)
//   确定 单击    进入/执行焦点项
//   确定 长按    返回上一级(事件对话框中禁用)
//
// 按键回调只入队;游戏在其按键任务内加 LVGL 锁消费事件并渲染。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "darkroom_app.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    darkroom_app_key(btn, ev);  // 内部带队列,非阻塞
}

void app_main(void) {
    ESP_LOGI(TAG, "小黑屋启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();

    // 电量计:顶栏显示用;失败不阻塞游戏(界面显示 --%)
    esp_err_t batt_err = bsp_battery_init();
    if (batt_err != ESP_OK)
        ESP_LOGW(TAG, "电量计初始化失败:%s(顶栏将显示 --%%)",
                 esp_err_to_name(batt_err));

    // 竖屏 240×320(定稿);横屏改法见 git 历史(硬件 MADCTL 旋转)。
    bsp_display_set_rotation(false, false, false);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,游戏无法操作");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        darkroom_app_enter();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "获取 LVGL 锁失败,页面未能创建(白屏的直接原因)");
    }

    ESP_LOGI(TAG, "就绪:三键操作,上下选择/确定执行/长按返回");
}
