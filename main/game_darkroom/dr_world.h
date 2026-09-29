// main/game_darkroom/dr_world.h —— M4 第三幕:世界与远征(对齐原版 world.js)。
// 数值口径 DESIGN.zh_CN.md §9.8。纯 C99,主机可测。
// 设备适配:世界由 map_seed 确定性生成(原版随机生成后整图落盘);
// 迷雾仅会话内,到访地标以 flags 持久点亮;远征中途断电按死亡。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr_state.h"
#include "dr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DR_WORLD_SIZE     61      // 原版 RADIUS 30 → 61×61
#define DR_WORLD_CENTER   30      // 村庄固定在正中

// 地块类型(切片一:地形+矿+哨站+星舰;老屋/洞穴/废镇/城市归切片二)
typedef enum {
    DR_WT_VILLAGE = 0,
    DR_WT_FOREST,       // 森林 15%
    DR_WT_FIELD,        // 田野 35%
    DR_WT_BARREN,       // 荒地 50%
    DR_WT_IRON,         // 铁矿(r5)
    DR_WT_COAL,         // 煤矿(r10)
    DR_WT_SULPHUR,      // 硫磺矿(r20)
    DR_WT_OUTPOST,      // 哨站(通往星舰的路上,补水)
    DR_WT_SHIP,         // 坠毁星舰(r28;M5)
} dr_world_tile_t;

// 世界与远征运行态(不落盘)
typedef struct {
    uint8_t tiles[DR_WORLD_SIZE * DR_WORLD_SIZE];
    uint8_t seen[(DR_WORLD_SIZE * DR_WORLD_SIZE + 7) / 8];   // 会话内迷雾
    bool    warned_thirst;    // 原版:断水第一次仅警告
    bool    warned_hunger;
    bool    visited_iron;     // 本次远征到访(回家时提交为 flags)
    bool    visited_coal;
    bool    visited_sulphur;
    bool    outpost_used;     // 哨站补水每远征一次
} dr_world_t;

// 世界生成:同一 map_seed 永远生成同一张图。
void dr_world_gen(dr_world_t *w, uint16_t seed);
uint8_t dr_world_tile(const dr_world_t *w, int x, int y);
bool dr_world_seen(const dr_world_t *w, int x, int y);
// 距村曼哈顿距离(危险半径判定用;≥8 无铁甲/≥18 无钢甲 = 危险)
uint16_t dr_world_home_dist(const dr_game_t *g);
bool dr_world_danger(const dr_game_t *g);

// 出发(原版 onArrival):需干肉 >0;水补满、HP 补满、带 min(干肉,10) 口。
bool dr_world_embark(dr_world_t *w, dr_game_t *g);

typedef enum {
    DR_MOVE_OK = 0,
    DR_MOVE_WARN_THIRST,   // 口渴难忍(下一次仍无水 = 死亡)
    DR_MOVE_WARN_HUNGER,   // 饥饿来袭
    DR_MOVE_DEATH,         // 死亡:远征物资全失,回家
    DR_MOVE_HOME,          // 踏上村庄格:提交矿 flags,余粮入库
    DR_MOVE_OUTPOST,       // 踏上哨站:水补满(每远征一次)
    DR_MOVE_IRON,          // 踏上铁矿(发现)
    DR_MOVE_COAL,
    DR_MOVE_SULPHUR,
    DR_MOVE_SHIP,          // 星舰(M5 占位)
    DR_MOVE_BLOCKED,       // 出界
} dr_move_result_t;

// 移动一格(东/南/西/北):迷雾点亮半径 2;水每步 1、干肉每 2 步 1;
// 断水/断粮先警告后死亡(原版 useSupplies 口径)。
dr_move_result_t dr_world_move(dr_world_t *w, dr_game_t *g, int dx, int dy);

// 远征中吃干肉:回 8 HP(原版 meat heal 8)
bool dr_world_eat(dr_game_t *g);

// 远征中断电/重启的兜底:物资已在出发时扣除,直接判死亡回家。
void dr_world_fail_trip(dr_world_t *w, dr_game_t *g);

// 上限:水 10(切片三:+水袋10/木桶20/水箱50);HP 10+皮5/铁15/钢35
uint8_t dr_world_water_cap(const dr_game_t *g);
uint8_t dr_world_health_cap(const dr_game_t *g);

#ifdef __cplusplus
}
#endif
