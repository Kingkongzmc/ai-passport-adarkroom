// main/game_darkroom/dr_state.c —— 存档打包/校验/迁移/离线时长钳制。
#include <string.h>

#include "dr_state.h"
#include "dr_util.h"

#define DR_SAVE_MAGIC 0x31535244u  // "DRS1" little-endian

// ---- v1 布局冻结副本(已发布固件的实际落盘结构;仅迁移/测试引用) ----
// v1 与 v2 的差异:res/res_total 各 13 槽(v2 为 14,皮革尾插)、
// 无尾部的 armor_lv/trap_bait_on/_rsv。改当前结构时不要动这里。
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

_Static_assert(sizeof(dr_game_v1_t) == 460u, "v1 body size must stay frozen");

_Static_assert(sizeof(dr_save_image_t) == sizeof(dr_save_hdr_t) + sizeof(dr_game_t),
               "save image must be header + body with no padding");

void dr_game_init(dr_game_t *g, uint32_t seed, uint32_t now_ts) {
    memset(g, 0, sizeof(*g));
    g->saved_at_ts = now_ts;
    g->rng_seed_state = (seed == 0u) ? 0xDEADBEEFu : seed;
    g->map_seed = (uint16_t)(seed >> 16);
    g->hero_hp = g->hero_hp_max = 10;   // 占位数值,M4 战斗表灌装时调整
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

    // v1 → v2:字段逐一搬运(资源枚举只尾插,下标语义不变;新字段补零)
    if (hdr->version == 1) {
        if (body_len != sizeof(dr_game_v1_t)) return false;
        const dr_game_v1_t *v1 = (const dr_game_v1_t *)(const void *)body;
        memset(out, 0, sizeof(*out));
        out->saved_at_ts = v1->saved_at_ts;
        memcpy(out->res, v1->res, sizeof(v1->res));
        memcpy(out->res_total, v1->res_total, sizeof(v1->res_total));
        out->population = v1->population;
        memcpy(out->job, v1->job, sizeof(v1->job));
        memcpy(out->building_lv, v1->building_lv, sizeof(v1->building_lv));
        memcpy(out->event_count, v1->event_count, sizeof(v1->event_count));
        out->flags = v1->flags;
        out->rng_seed_state = v1->rng_seed_state;
        out->fire_lv = v1->fire_lv;
        out->map_seed = v1->map_seed;
        out->hero_x = v1->hero_x;
        out->hero_y = v1->hero_y;
        out->hero_hp = v1->hero_hp;
        out->hero_hp_max = v1->hero_hp_max;
        out->in_wilderness = v1->in_wilderness;
        out->water = v1->water;
        out->food = v1->food;
        // armor_lv=0(无皮甲)、trap_bait_on=0(诱饵默认关)、_rsv=0
        return true;
    }
    return false;  // 未知历史版本
}

size_t dr_state_pack_v1(const dr_game_t *g, void *out, size_t outsz) {
    size_t need = sizeof(dr_save_hdr_t) + sizeof(dr_game_v1_t);
    if (!g || !out || outsz < need) return 0;
    dr_game_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.saved_at_ts = g->saved_at_ts;
    memcpy(v1.res, g->res, sizeof(v1.res));
    memcpy(v1.res_total, g->res_total, sizeof(v1.res_total));
    v1.population = g->population;
    memcpy(v1.job, g->job, sizeof(v1.job));
    memcpy(v1.building_lv, g->building_lv, sizeof(v1.building_lv));
    memcpy(v1.event_count, g->event_count, sizeof(v1.event_count));
    v1.flags = g->flags;
    v1.rng_seed_state = g->rng_seed_state;
    v1.fire_lv = g->fire_lv;
    v1.map_seed = g->map_seed;
    v1.hero_x = g->hero_x;
    v1.hero_y = g->hero_y;
    v1.hero_hp = g->hero_hp;
    v1.hero_hp_max = g->hero_hp_max;
    v1.in_wilderness = g->in_wilderness;
    v1.water = g->water;
    v1.food = g->food;

    dr_save_hdr_t *hdr = (dr_save_hdr_t *)out;
    hdr->magic = DR_SAVE_MAGIC;
    hdr->version = 1;
    hdr->_pad = 0;
    hdr->body_crc = dr_crc32(&v1, sizeof(v1));
    memcpy((uint8_t *)out + sizeof(dr_save_hdr_t), &v1, sizeof(v1));
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
