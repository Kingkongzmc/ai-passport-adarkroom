// main/game_darkroom/dr_state.c —— 存档打包/校验/迁移/离线时长钳制。
#include <string.h>

#include "dr_state.h"
#include "dr_util.h"

#define DR_SAVE_MAGIC 0x31535244u  // "DRS1" little-endian

// ---- 历史布局冻结副本(已发布固件的实际落盘结构;仅迁移/测试引用) ----
// v1(线上版):res 13 槽,无尾部装备字段。
// v2(未发布):res 14 槽(+皮革),尾部 armor_lv/trap_bait_on/_rsv;
//            职业枚举 LUMBER=0,HUNTER=1,TANNER=2,SMITH=3。
// v3(当前):  res 17 槽(+鳞/牙/布),尾插 temp_lv/builder_lv;
//            职业枚举重排 HUNTER=0,TRAPPER=1,TANNER=2,CHARCUTIER=3(对齐原版)。
// 改当前结构时不要动这里。
#pragma pack(push, 1)
typedef struct {
    uint32_t saved_at_ts;
    uint32_t res[13];
    uint32_t res_total[13];
    uint16_t population;
    uint16_t job[16];
    uint8_t  building_lv[40];
    uint16_t event_count[128];
    uint64_t flags;
    uint32_t rng_seed_state;
    uint8_t  fire_lv;
    uint16_t map_seed;
    uint8_t  hero_x, hero_y;
    uint8_t  hero_hp, hero_hp_max;
    uint8_t  in_wilderness;
    uint8_t  water, food;
} dr_game_v1_t;
#pragma pack(pop)

// v2(a8ba39a 时代固件,未发布):res 14 槽(+皮革),尾部 armor_lv/trap_bait_on/_rsv;
//            职业枚举 LUMBER=0,HUNTER=1,TANNER=2,SMITH=3。
//            注意:v2 固件的 dr_game_t 是自然对齐(未打包),实际落盘 480 字节
//            (flags 前有 2 字节对齐垫层,尾部补齐)——冻结副本必须同构,
//            按 472 打包会因长度不符把真机 v2 档误判坏档。
typedef struct {
    uint32_t saved_at_ts;
    uint32_t res[14];
    uint32_t res_total[14];
    uint16_t population;
    uint16_t job[16];
    uint8_t  building_lv[40];
    uint16_t event_count[128];
    uint64_t flags;
    uint32_t rng_seed_state;
    uint8_t  fire_lv;
    uint16_t map_seed;
    uint8_t  hero_x, hero_y;
    uint8_t  hero_hp, hero_hp_max;
    uint8_t  in_wilderness;
    uint8_t  water, food;
    uint8_t  armor_lv;
    uint8_t  trap_bait_on;
    uint8_t  _rsv[2];
} dr_game_v2_t;

_Static_assert(sizeof(dr_game_v1_t) == 460u, "v1 body size must stay frozen");
_Static_assert(sizeof(dr_game_v2_t) == 480u, "v2 body size must stay frozen(自然对齐)");
_Static_assert(sizeof(dr_save_image_t) == sizeof(dr_save_hdr_t) + sizeof(dr_game_t),
               "save image must be header + body with no padding");

void dr_game_init(dr_game_t *g, uint32_t seed, uint32_t now_ts) {
    memset(g, 0, sizeof(*g));
    g->saved_at_ts = now_ts;
    g->rng_seed_state = (seed == 0u) ? 0xDEADBEEFu : seed;
    g->map_seed = (uint16_t)(seed >> 16);
    g->hero_hp = g->hero_hp_max = 10;   // 占位数值,M4 战斗表灌装时调整
    g->res[DR_RES_WOOD] = 15;           // 原版起始木材 15(点火 5 木是唯一开局动作)
    g->res_total[DR_RES_WOOD] = 15;
}

void dr_state_pack(const dr_game_t *g, dr_save_image_t *img) {
    img->hdr.magic = DR_SAVE_MAGIC;
    img->hdr.version = DR_SAVE_VERSION;
    img->hdr._pad = 0;
    img->hdr.body_crc = dr_crc32(g, sizeof(*g));
    img->body = *g;
}

bool dr_state_unpack(const dr_save_image_t *img, dr_game_t *out,
                     uint16_t *out_version) {
    if (img->hdr.magic != DR_SAVE_MAGIC) return false;
    if (img->hdr.version != DR_SAVE_VERSION) return false;  // 历史版本走 dr_state_load
    if (img->hdr.body_crc != dr_crc32(&img->body, sizeof(img->body))) return false;
    *out = img->body;
    if (out_version) *out_version = img->hdr.version;
    return true;
}

// v2(14 槽/旧职业枚举)→ v3:资源尾插补零;职业按下标重映射
// (旧 LUMBER/SMITH 无对应原版职业,在编人数并入闲人=采集者);
// 温度按火焰档初始化;老档一律视为陌生人已恢复(builder 4),游戏可继续推进。
static void migrate_v2_to_v3(const dr_game_v2_t *v2, dr_game_t *out) {
    memset(out, 0, sizeof(*out));
    out->saved_at_ts = v2->saved_at_ts;
    memcpy(out->res, v2->res, sizeof(v2->res));
    memcpy(out->res_total, v2->res_total, sizeof(v2->res_total));
    out->population = v2->population;
    out->job[0] = v2->job[1];   // 旧 HUNTER(1) → 新 HUNTER(0)
    out->job[2] = v2->job[2];   // 旧 TANNER(2) → 新 TANNER(2);TRAPPER/CHARCUTIER 置 0
    memcpy(out->building_lv, v2->building_lv, sizeof(v2->building_lv));
    memcpy(out->event_count, v2->event_count, sizeof(v2->event_count));
    out->flags = v2->flags;
    out->rng_seed_state = v2->rng_seed_state;
    out->fire_lv = v2->fire_lv;
    out->map_seed = v2->map_seed;
    out->hero_x = v2->hero_x;
    out->hero_y = v2->hero_y;
    out->hero_hp = v2->hero_hp;
    out->hero_hp_max = v2->hero_hp_max;
    out->in_wilderness = v2->in_wilderness;
    out->water = v2->water;
    out->food = v2->food;
    out->armor_lv = v2->armor_lv;
    out->trap_bait_on = 0;      // v3 弃用
    out->temp_lv = (v2->fire_lv > 4u) ? 4u : v2->fire_lv;
    out->builder_lv = 4u;       // 老档默认陌生人已恢复帮忙
    // 老档森林必已解锁(有板车/陷阱/小屋任一即玩到过村庄);全新 v1 空档也无碍:
    // builder=4 时火焰日志不再触发开场剧情,由 flags 位 DR_FLAG_FOREST 兜底。
    if (v2->building_lv[0] || v2->building_lv[1] || v2->building_lv[2] ||
        v2->population > 0)
        out->flags |= (uint64_t)1u << 2;   // DR_FLAG_FOREST
}

// v1(13 槽)→ v3:先按 v2 口径搬运,再走 v2→v3 重映射。
static void migrate_v1_to_v3(const dr_game_v1_t *v1, dr_game_t *out) {
    dr_game_v2_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.saved_at_ts = v1->saved_at_ts;
    memcpy(v2.res, v1->res, sizeof(v1->res));
    memcpy(v2.res_total, v1->res_total, sizeof(v1->res_total));
    v2.population = v1->population;
    memcpy(v2.job, v1->job, sizeof(v1->job));
    memcpy(v2.building_lv, v1->building_lv, sizeof(v1->building_lv));
    memcpy(v2.event_count, v1->event_count, sizeof(v1->event_count));
    v2.flags = v1->flags;
    v2.rng_seed_state = v1->rng_seed_state;
    v2.fire_lv = v1->fire_lv;
    v2.map_seed = v1->map_seed;
    v2.hero_x = v1->hero_x;
    v2.hero_y = v1->hero_y;
    v2.hero_hp = v1->hero_hp;
    v2.hero_hp_max = v1->hero_hp_max;
    v2.in_wilderness = v1->in_wilderness;
    v2.water = v1->water;
    v2.food = v1->food;
    migrate_v2_to_v3(&v2, out);
}

bool dr_state_load(const void *blob, size_t len, dr_game_t *out) {
    if (!blob || !out) return false;
    if (len < sizeof(dr_save_hdr_t) + 16u || len > sizeof(dr_save_image_t))
        return false;
    const dr_save_hdr_t *hdr = (const dr_save_hdr_t *)blob;
    if (hdr->magic != DR_SAVE_MAGIC) return false;
    if (hdr->version == 0 || hdr->version > DR_SAVE_VERSION) return false;
    const uint8_t *body = (const uint8_t *)blob + sizeof(dr_save_hdr_t);
    size_t body_len = len - sizeof(dr_save_hdr_t);
    // CRC 按“实际存档的内容长度”验——旧版本镜像比当前结构短,
    // 按新尺寸算会把未存储的尾巴当垃圾判坏档(升级丢档事故点)。
    if (hdr->body_crc != dr_crc32(body, body_len)) return false;

    if (hdr->version == DR_SAVE_VERSION) {
        if (body_len != sizeof(dr_game_t)) return false;
        *out = *(const dr_game_t *)(const void *)body;
        return true;
    }
    if (hdr->version == 2) {
        if (body_len != sizeof(dr_game_v2_t)) return false;
        migrate_v2_to_v3((const dr_game_v2_t *)(const void *)body, out);
        return true;
    }
    if (hdr->version == 1) {
        if (body_len != sizeof(dr_game_v1_t)) return false;
        migrate_v1_to_v3((const dr_game_v1_t *)(const void *)body, out);
        return true;
    }
    return false;  // 未知历史版本
}

// ---- 主机测试用:压出历史布局镜像(验证迁移链) ----
static void fill_v1(const dr_game_t *g, dr_game_v1_t *v1) {
    memset(v1, 0, sizeof(*v1));
    v1->saved_at_ts = g->saved_at_ts;
    memcpy(v1->res, g->res, sizeof(v1->res));
    memcpy(v1->res_total, g->res_total, sizeof(v1->res_total));
    v1->population = g->population;
    memcpy(v1->job, g->job, sizeof(v1->job));
    memcpy(v1->building_lv, g->building_lv, sizeof(v1->building_lv));
    memcpy(v1->event_count, g->event_count, sizeof(v1->event_count));
    v1->flags = g->flags;
    v1->rng_seed_state = g->rng_seed_state;
    v1->fire_lv = g->fire_lv;
    v1->map_seed = g->map_seed;
    v1->hero_x = g->hero_x;
    v1->hero_y = g->hero_y;
    v1->hero_hp = g->hero_hp;
    v1->hero_hp_max = g->hero_hp_max;
    v1->in_wilderness = g->in_wilderness;
    v1->water = g->water;
    v1->food = g->food;
}

static void fill_v2(const dr_game_t *g, dr_game_v2_t *v2) {
    memset(v2, 0, sizeof(*v2));
    v2->saved_at_ts = g->saved_at_ts;
    memcpy(v2->res, g->res, sizeof(v2->res));
    memcpy(v2->res_total, g->res_total, sizeof(v2->res_total));
    v2->population = g->population;
    memcpy(v2->building_lv, g->building_lv, sizeof(v2->building_lv));
    memcpy(v2->event_count, g->event_count, sizeof(v2->event_count));
    v2->flags = g->flags;
    v2->rng_seed_state = g->rng_seed_state;
    v2->fire_lv = g->fire_lv;
    v2->map_seed = g->map_seed;
    v2->hero_x = g->hero_x;
    v2->hero_y = g->hero_y;
    v2->hero_hp = g->hero_hp;
    v2->hero_hp_max = g->hero_hp_max;
    v2->in_wilderness = g->in_wilderness;
    v2->water = g->water;
    v2->food = g->food;
    v2->res[13] = g->res[DR_RES_LEATHER];
    v2->res_total[13] = g->res_total[DR_RES_LEATHER];
    v2->armor_lv = g->armor_lv;
    v2->trap_bait_on = 0;
    // 职业按旧枚举重排(LUMBER=0,HUNTER=1,TANNER=2,SMITH=3)
    v2->job[1] = g->job[0];   // HUNTER
    v2->job[2] = g->job[2];   // TANNER
}

size_t dr_state_pack_v1(const dr_game_t *g, void *out, size_t outsz) {
    size_t need = sizeof(dr_save_hdr_t) + sizeof(dr_game_v1_t);
    if (!g || !out || outsz < need) return 0;
    dr_game_v1_t v1;
    fill_v1(g, &v1);
    dr_save_hdr_t *hdr = (dr_save_hdr_t *)out;
    hdr->magic = DR_SAVE_MAGIC;
    hdr->version = 1;
    hdr->_pad = 0;
    hdr->body_crc = dr_crc32(&v1, sizeof(v1));
    memcpy((uint8_t *)out + sizeof(dr_save_hdr_t), &v1, sizeof(v1));
    return need;
}

size_t dr_state_pack_v2(const dr_game_t *g, void *out, size_t outsz) {
    size_t need = sizeof(dr_save_hdr_t) + sizeof(dr_game_v2_t);
    if (!g || !out || outsz < need) return 0;
    dr_game_v2_t v2;
    fill_v2(g, &v2);
    dr_save_hdr_t *hdr = (dr_save_hdr_t *)out;
    hdr->magic = DR_SAVE_MAGIC;
    hdr->version = 2;
    hdr->_pad = 0;
    hdr->body_crc = dr_crc32(&v2, sizeof(v2));
    memcpy((uint8_t *)out + sizeof(dr_save_hdr_t), &v2, sizeof(v2));
    return need;
}

uint32_t dr_offline_ticks(dr_game_t *g, uint32_t now_ts) {
    if (now_ts < g->saved_at_ts) {  // 时钟回拨(RTC 丢失兜底):不产生负收益
        g->saved_at_ts = now_ts;
        return 0;
    }
    uint32_t delta = now_ts - g->saved_at_ts;

    uint64_t effective = (delta > DR_OFFLINE_CAP_S) ? DR_OFFLINE_CAP_S : delta;
    if (delta > DR_OFFLINE_CAP_S) {
        uint64_t over = delta - DR_OFFLINE_CAP_S;
        effective += (over * DR_OFFLINE_OVER_PERMILLE) / 1000u;
    }
    g->saved_at_ts = now_ts;
    return (uint32_t)(effective / DR_ECONOMY_TICK_S);
}
