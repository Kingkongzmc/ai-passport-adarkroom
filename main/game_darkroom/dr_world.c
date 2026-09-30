// main/game_darkroom/dr_world.c —— 世界生成与远征实现(对齐原版 world.js)。
#include <string.h>

#include "dr_world.h"
#include "dr_config.h"

static dr_rng_t *fight_rng(dr_game_t *g);   // 战斗随机流(定义在遭遇战段)
static uint8_t pick_enemy(const dr_world_t *w, const dr_game_t *g, dr_rng_t *r);

// 敌人表(原版 Encounters,2026-09-29 取证;定义见下方"遭遇战"段前的实体)
typedef struct {
    const char *name;
    uint8_t  dmg;        // 伤害
    uint16_t hit_pm;     // 命中(千分比)
    uint8_t  delay;      // 攻击间隔(回合;原版秒级冷却的离散映射)
    uint16_t hp;
    uint8_t  terrain;    // 出没地形
    uint16_t dist_min, dist_max;   // 距村曼哈顿档
    struct { uint8_t res; uint8_t min, max; uint16_t chance_pm; } loot[5];
} dr_enemy_def_t;
static const dr_enemy_def_t k_enemies[DR_ENEMY_KIND_COUNT];

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
    if (g->in_wilderness) return false;
    if (g->food == 0) return false;   // 必须已带干肉(原版:无干肉不可出发)
    memset(w->seen, 0, sizeof(w->seen));
    w->warned_thirst = w->warned_hunger = false;
    w->visited_iron = w->visited_coal = w->visited_sulphur = false;
    w->outpost_used = false;
    memset(w->loot, 0, sizeof(w->loot));
    w->fight_enemy = DR_ENEMY_KIND_COUNT;
    w->fight_round = 0;
    w->steps_since_fight = 0;
    trip_food_moves = 0;
    g->in_wilderness = 1;
    g->hero_x = DR_WORLD_CENTER;
    g->hero_y = DR_WORLD_CENTER;
    g->water = dr_world_water_cap(g);
    g->hero_hp = g->hero_hp_max = dr_world_health_cap(g);
    // 携带(干肉/药/子弹)已在出发界面从库存转入(food/carry_*)
    reveal(w, g->hero_x, g->hero_y);
    return true;
}

void dr_world_fail_trip(dr_world_t *w, dr_game_t *g) {
    (void)w;
    g->in_wilderness = 0;
    g->food = 0;
    g->water = 0;
    w->fight_enemy = DR_ENEMY_KIND_COUNT;
    w->carry_medicine = 0;
    w->carry_bullets = 0;
    memset(w->loot, 0, sizeof(w->loot));
}

static dr_move_result_t arrive_home(dr_world_t *w, dr_game_t *g) {
    if (w->visited_iron)    g->flags |= (uint64_t)1u << DR_FLAG_IRON_MINE;
    if (w->visited_coal)    g->flags |= (uint64_t)1u << DR_FLAG_COAL_MINE;
    if (w->visited_sulphur) g->flags |= (uint64_t)1u << DR_FLAG_SULPHUR_MINE;
    // 背袋全量入库(原版 goHome:干肉/药/子弹/战利品;水不回)
    g->res[DR_RES_FOOD] += g->food;
    g->res_total[DR_RES_FOOD] += g->food;
    g->res[DR_RES_MEDICINE] += w->carry_medicine;
    g->res_total[DR_RES_MEDICINE] += w->carry_medicine;
    g->res[DR_RES_BULLETS] += w->carry_bullets;
    g->res_total[DR_RES_BULLETS] += w->carry_bullets;
    for (int i = 0; i < DR_RES_KIND_COUNT; i++) {
        if (w->loot[i]) {
            g->res[i] += w->loot[i];
            g->res_total[i] += w->loot[i];
            w->loot[i] = 0;
        }
    }
    g->food = 0;
    g->water = 0;
    g->in_wilderness = 0;
    w->carry_medicine = 0;
    w->carry_bullets = 0;
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
    if (t >= DR_WT_FOREST && t <= DR_WT_BARREN) {
        // 普通地块:遭遇判定(距上一场 ≥3 步才有 20% 触发,原版口径;
        // 地标格不触发——进入地点优先)
        w->steps_since_fight++;
        if (w->steps_since_fight >= 3 &&
            dr_rng_chance(fight_rng(g), 200)) {
            uint8_t e = pick_enemy(w, g, fight_rng(g));
            if (e < DR_ENEMY_KIND_COUNT) {
                w->fight_enemy = e;
                w->fight_hp = k_enemies[e].hp;
                w->fight_round = 0;
                w->steps_since_fight = 0;
                return DR_MOVE_FIGHT;
            }
        }
        return DR_MOVE_OK;
    }
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

// ---- 敌人表(原版 Encounters,2026-09-29 取证;类型声明见文件头) ----

static const dr_enemy_def_t k_enemies[DR_ENEMY_KIND_COUNT] = {
    [DR_ENEMY_BEAST] = { "吼兽", 1, 800, 1, 5, DR_WT_FOREST, 0, 10,
        { {DR_RES_FUR,1,3,1000}, {DR_RES_MEAT,1,3,1000}, {DR_RES_TEETH,1,3,800} } },
    [DR_ENEMY_GAUNT] = { "瘦削男子", 2, 800, 2, 6, DR_WT_BARREN, 0, 10,
        { {DR_RES_CLOTH,1,3,800}, {DR_RES_TEETH,1,2,800}, {DR_RES_LEATHER,1,2,500} } },
    [DR_ENEMY_BIRD] = { "怪鸟", 3, 800, 2, 4, DR_WT_FIELD, 0, 10,
        { {DR_RES_SCALES,1,3,800}, {DR_RES_TEETH,1,2,500}, {DR_RES_MEAT,1,3,800} } },
    [DR_ENEMY_TWOHEAD] = { "双头兽", 2, 500, 3, 10, DR_WT_FIELD, 0, 10,
        { {DR_RES_FUR,2,4,1000}, {DR_RES_TEETH,2,3,800}, {DR_RES_MEAT,2,3,800} } },
    [DR_ENEMY_SHIVER] = { "颤抖男子", 5, 500, 1, 20, DR_WT_BARREN, 11, 20,
        { {DR_RES_CLOTH,1,1,200}, {DR_RES_TEETH,1,2,800},
          {DR_RES_LEATHER,1,1,200}, {DR_RES_MEDICINE,1,3,700} } },
    [DR_ENEMY_MANEATER] = { "食人魔", 3, 800, 1, 25, DR_WT_FOREST, 11, 20,
        { {DR_RES_FUR,5,10,1000}, {DR_RES_MEAT,5,10,1000}, {DR_RES_TEETH,5,10,800} } },
    [DR_ENEMY_SCAVENGER] = { "拾荒者", 4, 800, 2, 30, DR_WT_BARREN, 11, 20,
        { {DR_RES_CLOTH,5,10,800}, {DR_RES_LEATHER,5,10,800},
          {DR_RES_IRON,1,5,500}, {DR_RES_MEDICINE,1,2,100} } },
    [DR_ENEMY_LIZARD] = { "巨蜥", 5, 800, 2, 20, DR_WT_FIELD, 11, 20,
        { {DR_RES_SCALES,5,10,800}, {DR_RES_TEETH,5,10,500}, {DR_RES_MEAT,5,10,800} } },
    [DR_ENEMY_TERROR] = { "狂野恐怖", 6, 800, 1, 45, DR_WT_FOREST, 21, 99,
        { {DR_RES_FUR,5,10,1000}, {DR_RES_MEAT,5,10,1000}, {DR_RES_TEETH,5,10,800} } },
    [DR_ENEMY_SOLDIER] = { "士兵", 8, 800, 2, 50, DR_WT_BARREN, 21, 99,
        { {DR_RES_CLOTH,5,10,800}, {DR_RES_BULLETS,1,5,500}, {DR_RES_MEDICINE,1,2,100} } },
    [DR_ENEMY_SNIPER] = { "狙击手", 15, 800, 4, 30, DR_WT_FIELD, 21, 99,
        { {DR_RES_CLOTH,5,10,800}, {DR_RES_BULLETS,1,5,500}, {DR_RES_MEDICINE,1,2,100} } },
};

const char *dr_enemy_name(uint8_t enemy) {
    return (enemy < DR_ENEMY_KIND_COUNT) ? k_enemies[enemy].name : "?";
}

uint8_t dr_world_weapon_dmg(uint8_t weapon_lv) {
    // 原版武器表:拳1/骨矛2/铁剑4/钢剑6/步枪5(耗弹)
    static const uint8_t dmg[5] = { 1, 2, 4, 6, 5 };
    return (weapon_lv < 5) ? dmg[weapon_lv] : 1;
}

// ---- 背袋承重(原版 Path.Weight;单位 0.1) ----
uint16_t dr_world_bag_weight(const dr_game_t *g, const dr_world_t *w) {
    uint64_t weight = (uint64_t)g->food * 10u + (uint64_t)w->carry_medicine * 10u +
                      (uint64_t)w->carry_bullets;
    for (int i = 0; i < DR_RES_KIND_COUNT; i++) weight += (uint64_t)w->loot[i] * 10u;
    return (uint16_t)(weight > 0xFFFFu ? 0xFFFFu : weight);
}

uint16_t dr_world_carry_medicine(const dr_world_t *w) { return w->carry_medicine; }
uint16_t dr_world_carry_bullets(const dr_world_t *w)  { return w->carry_bullets; }

bool dr_world_outfit_add(dr_world_t *w, dr_game_t *g, uint8_t res, int16_t delta) {
    if (delta == 0) return false;
    // 出发前在仓库与背袋间调配;干肉直接对应 g->food
    uint16_t *carry = NULL;
    uint32_t *store = NULL;
    uint32_t unit_w = 10;   // 0.1 单位
    if (res == DR_RES_FOOD)       { store = &g->res[DR_RES_FOOD]; }
    else if (res == DR_RES_MEDICINE) { store = &g->res[DR_RES_MEDICINE];
                                       carry = &w->carry_medicine; }
    else if (res == DR_RES_BULLETS)  { store = &g->res[DR_RES_BULLETS];
                                       carry = &w->carry_bullets; unit_w = 1; }
    else return false;

    if (delta > 0) {
        if (*store < (uint32_t)delta) return false;
        uint64_t weight = dr_world_bag_weight(g, w);
        for (int i = 0; i < delta; i++) {
            if (weight + unit_w > DR_BAG_CAP_TENTHS) return i > 0;
            weight += unit_w;
            (*store)--;
            if (carry) (*carry)++;
            else g->food++;
        }
        return true;
    }
    int16_t back = -delta;
    if (carry) {
        if (*carry < (uint32_t)back) back = (int16_t)*carry;
        *carry -= (uint16_t)back;
    } else {
        if ((uint32_t)back > g->food) back = g->food;
        g->food -= (uint8_t)back;
    }
    *store += (uint32_t)back;
    return true;
}

// ---- 遭遇战 ----
// 战斗专用随机流(会话内;断电=死亡,无需落盘)
static dr_rng_t s_fight_rng;
static bool s_fight_rng_ready = false;

static dr_rng_t *fight_rng(dr_game_t *g) {
    if (!s_fight_rng_ready) {
        dr_rng_seed(&s_fight_rng, g->map_seed * 77u + 13u);
        s_fight_rng_ready = true;
    }
    return &s_fight_rng;
}

bool dr_world_fight_active(const dr_world_t *w) {
    return w->fight_enemy < DR_ENEMY_KIND_COUNT;
}

uint16_t dr_world_fight_hp(const dr_world_t *w) {
    return dr_world_fight_active(w) ? w->fight_hp : 0;
}

uint16_t dr_world_fight_hp_max(const dr_world_t *w) {
    return dr_world_fight_active(w) ? k_enemies[w->fight_enemy].hp : 0;
}

uint8_t dr_world_fight_enemy(const dr_world_t *w, const dr_game_t *g) {
    (void)g;
    return w->fight_enemy;
}

static uint8_t pick_enemy(const dr_world_t *w, const dr_game_t *g, dr_rng_t *r) {
    uint8_t terrain = w->tiles[g->hero_y * DR_WORLD_SIZE + g->hero_x];
    uint16_t dist = dr_world_home_dist(g);
    uint8_t cands[DR_ENEMY_KIND_COUNT];
    int n = 0;
    for (int i = 0; i < DR_ENEMY_KIND_COUNT; i++) {
        const dr_enemy_def_t *e = &k_enemies[i];
        if (e->terrain == terrain && dist >= e->dist_min && dist <= e->dist_max)
            cands[n++] = (uint8_t)i;
    }
    return (n == 0) ? DR_ENEMY_KIND_COUNT : cands[dr_rng_below(r, (uint32_t)n)];
}

static void enemy_turn(dr_world_t *w, dr_game_t *g, dr_rng_t *r,
                       dr_fight_result_t *out) {
    const dr_enemy_def_t *e = &k_enemies[w->fight_enemy];
    w->fight_round++;
    if (w->fight_round % e->delay != 0) { *out = DR_FIGHT_ENEMY_SKIP; return; }
    if (dr_rng_below(r, 1000u) >= e->hit_pm) { *out = DR_FIGHT_ENEMY_MISS; return; }
    if (g->hero_hp > e->dmg) g->hero_hp -= e->dmg;
    else {
        g->hero_hp = 0;
        dr_world_fail_trip(w, g);
        *out = DR_FIGHT_LOSE;
        return;
    }
    *out = DR_FIGHT_ENEMY_HIT;
}

static void fight_loot(dr_world_t *w, dr_game_t *g, dr_rng_t *r) {
    const dr_enemy_def_t *e = &k_enemies[w->fight_enemy];
    for (int i = 0; i < 5; i++) {
        if (e->loot[i].max == 0) break;
        if (!dr_rng_chance(r, e->loot[i].chance_pm)) continue;
        uint32_t n = e->loot[i].min +
                     dr_rng_below(r, (uint32_t)(e->loot[i].max - e->loot[i].min + 1));
        for (uint32_t k = 0; k < n; k++) {
            // 战利品入包(承重满即弃,原版口径)
            if (dr_world_bag_weight(g, w) + 10u > DR_BAG_CAP_TENTHS) return;
            w->loot[e->loot[i].res]++;
        }
    }
}

dr_fight_result_t dr_world_fight_attack(dr_world_t *w, dr_game_t *g) {
    if (!dr_world_fight_active(w)) return DR_FIGHT_NONE;
    dr_rng_t *r = fight_rng(g);

    uint8_t dmg = dr_world_weapon_dmg(g->weapon_lv);
    if (g->weapon_lv == 4) {                       // 步枪:耗 1 子弹,无弹退化为拳
        if (w->carry_bullets > 0) w->carry_bullets--;
        else dmg = 1;
    }
    if (!dr_rng_chance(r, 800)) {                  // 玩家命中 80%(原版)
        dr_fight_result_t out;
        enemy_turn(w, g, r, &out);
        return out;
    }
    if (w->fight_hp > dmg) w->fight_hp -= dmg;
    else {
        fight_loot(w, g, r);
        w->fight_enemy = DR_ENEMY_KIND_COUNT;
        return DR_FIGHT_WIN;
    }
    dr_fight_result_t out;
    enemy_turn(w, g, r, &out);
    return out;
}

dr_fight_result_t dr_world_fight_eat(dr_world_t *w, dr_game_t *g) {
    if (!dr_world_fight_active(w) || g->food == 0) return DR_FIGHT_NONE;
    g->food--;
    uint8_t cap = dr_world_health_cap(g);
    g->hero_hp = (g->hero_hp > cap - 8u) ? cap : (uint8_t)(g->hero_hp + 8u);
    dr_fight_result_t out;
    enemy_turn(w, g, fight_rng(g), &out);
    return out;
}

dr_fight_result_t dr_world_fight_medicine(dr_world_t *w, dr_game_t *g) {
    if (!dr_world_fight_active(w) || w->carry_medicine == 0) return DR_FIGHT_NONE;
    w->carry_medicine--;
    uint8_t cap = dr_world_health_cap(g);
    g->hero_hp = ((uint16_t)g->hero_hp + 20u >= cap) ? cap
                                                     : (uint8_t)(g->hero_hp + 20u);
    dr_fight_result_t out;
    enemy_turn(w, g, fight_rng(g), &out);
    return out;
}

dr_fight_result_t dr_world_fight_flee(dr_world_t *w, dr_game_t *g) {
    if (!dr_world_fight_active(w)) return DR_FIGHT_NONE;
    dr_rng_t *r = fight_rng(g);
    if (dr_rng_chance(r, 800)) {                   // 逃跑 80% 成功
        w->fight_enemy = DR_ENEMY_KIND_COUNT;
        return DR_FIGHT_FLED;
    }
    dr_fight_result_t out;
    enemy_turn(w, g, r, &out);
    return out;
}

// 远征中吃干肉:回 8 HP(原版 meat heal 8)
bool dr_world_eat(dr_game_t *g) {
    if (!g->in_wilderness || g->food == 0) return false;
    g->food--;
    uint8_t cap = dr_world_health_cap(g);
    g->hero_hp = (g->hero_hp > cap - 8u) ? cap : (uint8_t)(g->hero_hp + 8u);
    return true;
}
