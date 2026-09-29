# 独立应用:开机直接进入《小黑屋》。
# 引擎(game_darkroom)为纯逻辑,与 IDF/LVGL 解耦;darkroom_app 是 LVGL 壳,
# dr_port 负责存档的 NVS 落盘。
idf_component_register(
    SRCS "main.c"
         "darkroom_app.c"
         "dr_port.c"
         "game_darkroom/dr_rules.c"
         "game_darkroom/dr_state.c"
         "game_darkroom/dr_world.c"
         "game_darkroom/dr_events.c"
         "game_darkroom/dr_events_data.c"
         "game_darkroom/dr_text.c"
         "game_darkroom/dr_util.c"
         "../assets/fonts/dr_font_12.c"
         "../assets/fonts/dr_font_16.c"
         "../assets/fonts/dr_font_24.c"
    INCLUDE_DIRS "." "game_darkroom"
    REQUIRES bsp esp_timer esp_hw_support nvs_flash
)
