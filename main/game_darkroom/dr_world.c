// main/game_darkroom/dr_world.c —— 世界生成与远征实现(对齐原版 world.js)。
#include <string.h>

#include "dr_world.h"
#include "dr_config.h"

// ---- 地形生成(原版:森林15/田野35/荒地50,粘滞 0.5;村庄必在森林中) ----
// 设备适配:原版螺旋逐格 + 邻格加权;此处行主序 + 上/左邻格 50% 延续,
// 同样形成自然斑块,且由种子确定。
static void gen_terrain(dr_world_t *w, dr_rng_t *r) {
    for (int y = 0; y < DR_WORLD_SIZE; y++) {
        for (int x = 0; x < DR_WORLD_SIZE; x++) {
            uint8_t t;
            if (dr_rng_below(r, 2) == 0) {          // stickiness 0.5
                uint8_t up = (y > 0) ? w->tiles[(y - 1) * DR_WORLD_SIZE + x]
                                     : DR_WT_BARREN;
                uint8_t left = (x > 0) ? w->tiles[y * DR_WORLD_SIZE + x - 1]
                                       : DR_WT_BARREN;
                uint8_t nb = (dr_rng_below(r, 2) == 0) ? up : left;
                t = (nb <= DR_WT_BARREN) ? nb : DR_WT_BARREN;
            } else {
                uint32_t roll = dr_rng_below(r, 100);
                t = (roll < 15) ? DR_WT_FOREST
                  : (roll < 50) ? DR_WT_FIELD
                                : DR_WT_BARREN;
            }
            w->tiles[y * DR_WORLD_SIZE + x] = t;
        }
    }
    // 村庄固定在正中,周边一圈保持森林(原版村庄必在森林里)
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            w->tiles[(DR_WORLD_CENTER + dy) * DR_WORLD_SIZE +
                     DR_WORLD_CENTER + dx] = DR_WT_FOREST;
        }
    w->tiles[DR_WORLD_CENTER * DR_WORLD_SIZE + DR_WORLD_CENTER] = DR_WT_VILLAGE;
}

// 在距村曼哈顿半径 [r-1, r+1] 的环带上放一个地标(避开村庄/已放地标)
static bool place_landmark(dr_world_t *w, dr_rng_t *r, uint8_t tile,
                           uint16_t ring) {
    for (int tries = 0; tries < 512; tries++) {
        int dx = (int)dr_rng_below(r, 2u * ring + 1u) - (int)ring;
        int dy = (int)dr_rng_below(r, 2u * ring + 1u) - (int)ring;
        int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (dist < (int)ring - 1 || dist > (int)ring + 1) continue;
        int x = DR_WORLD_CENTER + dx, y = DR_WORLD_CENTER + dy;
        if (x < 1 || y < 1 || x >= DR_WORLD_SIZE - 1 ||
            y >= DR_WORLD_SIZE - 1) continue;
        uint8_t *p = &w->tiles[y * DR_WORLD_SIZE + x];
        if (*p == DR_WT_VILLAGE || *p >= DR_WT_IRON) continue;
        *p = tile;
        return true;
    }
    return false;
}

void dr_world_gen(dr_world_t *w, uint16_t seed) {
    memset(w, 0, sizeof(*w));
    dr_rng_t r;
    dr_rng_seed(&r, (uint32_t)seed * 2654435761u + 1u);
    gen_terrain(w, &r);
    // 矿(原版 num/r:铁 1@5,煤 1@10,硫 1@20)
    place_landmark(w, &r, DR_WT_IRON, 5);
    place_landmark(w, &r, DR_WT_COAL, 10);
    place_landmark(w, &r, DR_WT_SULPHUR, 20);
    // 星舰(r28)与补给路:哨站只在通往星舰的路上(原版),间隔 8 格补水
    int sx = (dr_rng_below(&r, 2) == 0) ? 1 : -1;
    int sy = (dr_rng_below(&r, 2) == 0) ? 1 : -1;
    static const int stops[3] = { 8, 16, 24 };
    for (int i = 0; i < 3; i++) {
        int half = stops[i] / 2;
        int x = DR_WORLD_CENTER + half * sx;
        int y = DR_WORLD_CENTER + (stops[i] - half) * sy;
        w->tiles[y * DR_WORLD_SIZE + x] = DR_WT_OUTPOST;
    }
    w->tiles[(DR_WORLD_CENTER + 14 * sy) * DR_WORLD_SIZE +
             DR_WORLD_CENTER + 14 * sx] = DR_WT_SHIP;   // 14+14=28
}

uint8_t dr_world_tile(const dr_world_t *w, int x, int y) {
    if (x < 0 || y < 0 || x >= DR_WORLD_SIZE || y >= DR_WORLD_SIZE)
        return DR_WT_BARREN;
    return w->tiles[y * DR_WORLD_SIZE + x];
}

bool dr_world_seen(const dr_world_t *w, int x, int y) {
    if (x < 0 || y < 0 || x >= DR_WORLD_SIZE || y >= DR_WORLD_SIZE)
        return false;
    uint32_t i = (uint32_t)y * DR_WORLD_SIZE + x;
    return (w->seen[i / 8] >> (i % 8)) & 1u;
}

static void reveal(dr_world_t *w, int x, int y) {
    // 原版 LIGHT_RADIUS 2(菱形):|dx|+|dy| <= 2
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > 2) continue;
            int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= DR_WORLD_SIZE || py >= DR_WORLD_SIZE)
                continue;
            uint32_t i = (uint32_t)py * DR_WORLD_SIZE + px;
            w->seen[i / 8] |= (uint8_t)(1u << (i % 8));
        }
}

uint16_t dr_world_home_dist(const dr_game_t *g) {
    int dx = (int)g->hero_x - DR_WORLD_CENTER;
    int dy = (int)g->hero_y - DR_WORLD_CENTER;
    return (uint16_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

bool dr_world_danger(const dr_game_t *g) {
    uint16_t d = dr_world_home_dist(g);
    if (d >= 18u && g->armor_lv < 3u) return true;   // 需钢甲
    if (d >= 8u && g->armor_lv < 2u) return true;    // 需铁甲
    return false;
}

uint8_t dr_world_water_cap(const dr_game_t *g) {
    (void)g;
    return 10;   // 切片三:+水袋10/木桶20/水箱50
}

uint8_t dr_world_health_cap(const dr_game_t *g) {
    static const uint8_t bonus[4] = { 0, 5, 15, 35 };   // 无/皮/铁/钢
    uint8_t a = (g->armor_lv > 3u) ? 3u : g->armor_lv;
    return 10 + bonus[a];
}

// ---- 远征 ----
static uint16_t trip_food_moves;   // 每 2 步吃 1 干肉的步计数(会话内)

bool dr_world_embark(dr_world_t *w, dr_game_t *g) {
    if (g->res[DR_RES_FOOD] == 0) return false;
    if (g->in_wilderness) return false;
    memset(w->seen, 0, sizeof(w->seen));
    w->warned_thirst = w->warned_hunger = false;
    w->visited_iron = w->visited_coal = w->visited_sulphur = false;
    w->outpost_used = false;
    trip_food_moves = 0;
    g->in_wilderness = 1;
    g->hero_x = DR_WORLD_CENTER;
    g->hero_y = DR_WORLD_CENTER;
    g->water = dr_world_water_cap(g);
    g->hero_hp = g->hero_hp_max = dr_world_health_cap(g);
    // 带上干肉(基础负重 10 口;出发即从库存扣,死亡即失,回家入库)
    uint32_t carry = g->res[DR_RES_FOOD] > 10u ? 10u : g->res[DR_RES_FOOD];
    g->res[DR_RES_FOOD] -= carry;
    g->food = (uint8_t)carry;
    reveal(w, g->hero_x, g->hero_y);
    return true;
}

void dr_world_fail_trip(dr_world_t *w, dr_game_t *g) {
    (void)w;
    g->in_wilderness = 0;
    g->food = 0;
    g->water = 0;
}

static dr_move_result_t arrive_home(dr_world_t *w, dr_game_t *g) {
    if (w->visited_iron)    g->flags |= (uint64_t)1u << DR_FLAG_IRON_MINE;
    if (w->visited_coal)    g->flags |= (uint64_t)1u << DR_FLAG_COAL_MINE;
    if (w->visited_sulphur) g->flags |= (uint64_t)1u << DR_FLAG_SULPHUR_MINE;
    g->res[DR_RES_FOOD] += g->food;          // 余粮入库(原版 goHome)
    g->res_total[DR_RES_FOOD] += g->food;
    g->food = 0;
    g->water = 0;
    g->in_wilderness = 0;
    return DR_MOVE_HOME;
}

dr_move_result_t dr_world_move(dr_world_t *w, dr_game_t *g, int dx, int dy) {
    if (!g->in_wilderness) return DR_MOVE_BLOCKED;
    int nx = (int)g->hero_x + dx, ny = (int)g->hero_y + dy;
    if (nx < 0 || ny < 0 || nx >= DR_WORLD_SIZE || ny >= DR_WORLD_SIZE)
        return DR_MOVE_BLOCKED;
    g->hero_x = (uint8_t)nx;
    g->hero_y = (uint8_t)ny;
    reveal(w, nx, ny);

    // 原版 useSupplies:水每步 1;食每 2 步 1;断供先警告后死亡
    if (g->water > 0) g->water--;
    else if (w->warned_thirst) { dr_world_fail_trip(w, g); return DR_MOVE_DEATH; }
    else { w->warned_thirst = true; return DR_MOVE_WARN_THIRST; }
    trip_food_moves++;
    if (trip_food_moves >= 2) {
        trip_food_moves = 0;
        if (g->food > 0) g->food--;
        else if (w->warned_hunger) { dr_world_fail_trip(w, g); return DR_MOVE_DEATH; }
        else { w->warned_hunger = true; return DR_MOVE_WARN_HUNGER; }
    }

    uint8_t t = w->tiles[ny * DR_WORLD_SIZE + nx];
    switch ((dr_world_tile_t)t) {
        case DR_WT_VILLAGE:  return arrive_home(w, g);
        case DR_WT_OUTPOST:
            if (!w->outpost_used) {
                w->outpost_used = true;
                g->water = dr_world_water_cap(g);
                return DR_MOVE_OUTPOST;
            }
            return DR_MOVE_OK;
        case DR_WT_IRON:     w->visited_iron = true;     return DR_MOVE_IRON;
        case DR_WT_COAL:     w->visited_coal = true;     return DR_MOVE_COAL;
        case DR_WT_SULPHUR:  w->visited_sulphur = true;  return DR_MOVE_SULPHUR;
        case DR_WT_SHIP:     return DR_MOVE_SHIP;
        default:             return DR_MOVE_OK;
    }
}

bool dr_world_eat(dr_game_t *g) {
    if (!g->in_wilderness || g->food == 0) return false;
    g->food--;
    uint8_t cap = dr_world_health_cap(g);
    g->hero_hp = (g->hero_hp > cap - 8u) ? cap : (uint8_t)(g->hero_hp + 8u);
    return true;
}
