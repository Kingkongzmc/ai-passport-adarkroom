// main/game_darkroom/dr_state.c —— 存档打包/校验/迁移/离线时长钳制。
#include <string.h>

#include "dr_state.h"
#include "dr_util.h"

#define DR_SAVE_MAGIC 0x31535244u  // "DRS1" little-endian

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
    if (img->hdr.version == 0 || img->hdr.version > DR_SAVE_VERSION) return false;
    if (img->hdr.body_crc != dr_crc32(&img->body, sizeof(img->body))) return false;
    *out = img->body;
    if (out_version) *out_version = img->hdr.version;
    return true;
}

bool dr_state_migrate(dr_game_t *g, uint16_t from_version) {
    (void)g;  // v1 起字段以零填充即可升级;后续版本在这里逐级换算
    // 逐级迁移:每个 case 把 from_version 升一级,新字段已在 unpack 时为零。
    // v1 是当前版本,暂无历史版本需要迁移;保留骨架供后续版本演进。
    uint16_t v = from_version;
    while (v < DR_SAVE_VERSION) {
        switch (v) {
            case 1:
                v++;
                break;
            default:
                return false;  // 未知版本(不应发生,unpack 已挡)
        }
    }
    return true;
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
