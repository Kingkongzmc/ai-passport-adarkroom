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
    DR_WT_HOUSE,        // 老屋 ×10(r0-45;药/补给/遭遇)
    DR_WT_CAVE,         // 潮湿洞穴 ×5(r3-10;需火把)
    DR_WT_TOWN,         // 废镇 ×10(r10-20)
    DR_WT_CITY,         // 废墟城市 ×20(r20-45)
    DR_WT_KIND_COUNT,
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
    // 携带与会话内背袋(药/子弹携带量;杂项战利品按资源入包,回家入库)
    uint16_t carry_medicine;
    uint16_t carry_bullets;
    uint32_t loot[DR_RES_KIND_COUNT];
    // 遭遇战状态
    uint8_t  fight_enemy;     // dr_enemy_t;KIND_COUNT=无
    uint16_t fight_hp;
    uint16_t fight_round;     // 敌人按 attackDelay 逢倍数回合反击
    uint16_t steps_since_fight;
    // 地点状态
    uint8_t  location;        // 当前地点 tile;0=不在地点
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
    DR_MOVE_FIGHT,         // 遭遇战触发(进入战斗页)
    DR_MOVE_HOUSE,         // 踏上老屋(进入地点页)
    DR_MOVE_CAVE,          // 踏上潮湿洞穴
    DR_MOVE_TOWN,          // 踏上废镇
    DR_MOVE_CITY,          // 踏上废墟城市
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

// 出发携带(Path 承重制,重量单位=0.1):干肉/药 1.0,子弹 0.1,基础容量 10.0
#define DR_BAG_CAP_TENTHS  100u
uint16_t dr_world_bag_weight(const dr_game_t *g, const dr_world_t *w);
// 携带量(药/子弹在会话内;干肉即 g->food)
uint16_t dr_world_carry_medicine(const dr_world_t *w);
uint16_t dr_world_carry_bullets(const dr_world_t *w);
bool dr_world_outfit_add(dr_world_t *w, dr_game_t *g, uint8_t res, int16_t delta);

// ---- 遭遇战(原版 Encounters:距离三档 × 地形) ----
typedef enum {
    DR_ENEMY_BEAST = 0,     // 吼兽 d≤10 森林
    DR_ENEMY_GAUNT,         // 瘦削男子 d≤10 荒地
    DR_ENEMY_BIRD,          // 怪鸟 d≤10 田野
    DR_ENEMY_TWOHEAD,       // 双头兽 d≤10 田野
    DR_ENEMY_SHIVER,        // 颤抖男子 10<d≤20 荒地
    DR_ENEMY_MANEATER,      // 食人魔 10<d≤20 森林
    DR_ENEMY_SCAVENGER,     // 拾荒者 10<d≤20 荒地
    DR_ENEMY_LIZARD,        // 巨蜥 10<d≤20 田野
    DR_ENEMY_TERROR,        // 狂野恐怖 d>20 森林
    DR_ENEMY_SOLDIER,       // 士兵 d>20 荒地
    DR_ENEMY_SNIPER,        // 狙击手 d>20 田野
    DR_ENEMY_KIND_COUNT,
} dr_enemy_t;

const char *dr_enemy_name(uint8_t enemy);        // 中文名(UI/日志)
uint8_t dr_world_fight_enemy(const dr_world_t *w, const dr_game_t *g);
                                                // 当前遭遇的敌人(无战斗=KIND_COUNT)

// 战斗行为(回合制设备适配):每回合玩家行动一次,敌人按 attackDelay
// 逢其倍数回合反击(原版为实时冷却,数值同源)。
typedef enum {
    DR_FIGHT_NONE = 0,
    DR_FIGHT_WIN,           // 胜利:战利品已入包(重量超容部分丢弃)
    DR_FIGHT_LOSE,          // 战败:同死亡口径
    DR_FIGHT_FLED,          // 逃跑成功(80%)
    DR_FIGHT_MISS,          // 玩家未命中
    DR_FIGHT_HIT,           // 命中(敌人剩余血见 fight_hp)
    DR_FIGHT_ENEMY_HIT,     // 敌人命中
    DR_FIGHT_ENEMY_MISS,
    DR_FIGHT_ENEMY_SKIP,    // 敌人本回合未到攻击间隔
} dr_fight_result_t;

bool     dr_world_fight_active(const dr_world_t *w);
uint16_t dr_world_fight_hp(const dr_world_t *w);
uint16_t dr_world_fight_hp_max(const dr_world_t *w);
dr_fight_result_t dr_world_fight_attack(dr_world_t *w, dr_game_t *g);
dr_fight_result_t dr_world_fight_eat(dr_world_t *w, dr_game_t *g);      // 干肉 +8
dr_fight_result_t dr_world_fight_medicine(dr_world_t *w, dr_game_t *g);  // +20 HP
dr_fight_result_t dr_world_fight_flee(dr_world_t *w, dr_game_t *g);
// 武器伤害(原版武器表;步枪耗 1 子弹)
uint8_t dr_world_weapon_dmg(uint8_t weapon_lv);

// ---- 地点搜索(设备适配"单次搜索"模型,DESIGN §9.8) ----
uint16_t dr_world_bag_cap(const dr_game_t *g);   // 背袋容量(基础10+背具,0.1)
// 当前所在地点(无=不在地点);踏入地标格时设置,离开地点页清零。
uint8_t dr_world_location(const dr_world_t *w);       // dr_world_tile_t
const char *dr_world_location_name(uint8_t tile);
void dr_world_location_leave(dr_world_t *w);
typedef enum {
    DR_LOC_NONE = 0,
    DR_LOC_LOOT,        // 搜到物资(已入包;详情看返回的 loot 摘要)
    DR_LOC_WATER,       // 找到水(已补水)+可能有物资
    DR_LOC_FIGHT,       // 触发战斗(fight_* 已就绪,进战斗页)
    DR_LOC_EMPTY,       // 一无所获
    DR_LOC_NEED_TORCH,  // 需要火把才能深入
} dr_loc_result_t;
// 搜索当前地点一次(洞穴消耗火把标记);loot 文案摘要写入 out(可 NULL)
dr_loc_result_t dr_world_location_search(dr_world_t *w, dr_game_t *g,
                                         char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
