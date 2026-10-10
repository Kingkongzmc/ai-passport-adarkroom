// main/game_darkroom/dr_savecode.c —— 存档码实现,格式见 dr_savecode.h。
#include "dr_savecode.h"

#include <string.h>

#include "dr_util.h"

#define DR_CODE_MAGIC_0 'D'
#define DR_CODE_MAGIC_1 'K'
#define DR_CODE_PAYLOAD_MAX 256u   // 防御上限(实际最坏 176;越界即坏帧)

// ---- CRC8:poly 0x07,init 0x00,无反射,无异或出(CRC-8/ATM) ----
// 流式:帧头(5B)与载荷在帧里被 CRC 字节隔开,分两段喂同一状态。
static uint8_t crc8_update(uint8_t c, const uint8_t *p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    return c;
}

static uint8_t crc8_head(const uint8_t *head5, const uint8_t *payload,
                         size_t plen) {
    return crc8_update(crc8_update(0, head5, 5), payload, plen);
}

// ---- 零游程 RLE ----
// 流 = 段序列,每段 1 字节 tag:
//   tag < 0x80:字面段,后随 tag+1(1..128)个原样字节
//   tag >= 0x80:零游程,长度 = tag-0x80+1(1..128)个 0x00
// 贪心:连续零 >=2 发零游程段(≤128),否则并入字面段;字面段满 128 即发。
static size_t rle_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0, i = 0;
    while (i < n) {
        if (in[i] == 0) {
            size_t z = 0;
            while (i + z < n && in[i + z] == 0 && z < 128) z++;
            if (z >= 2) {                          // 单零并入字面更省
                if (o + 1 > cap) return 0;
                out[o++] = (uint8_t)(0x80u + z - 1);
                i += z;
                continue;
            }
        }
        // 字面段: collecting 直到 128 上限/下一个 ≥2 零游程
        size_t run = 0;
        while (i + run < n && run < 128) {
            if (in[i + run] == 0) {
                size_t z = 0;
                while (i + run + z < n && in[i + run + z] == 0) z++;
                if (z >= 2) break;                 // 留给零游程段
                run += z;                          // 单零并入字面
            } else {
                run++;
            }
        }
        if (o + 1 + run > cap) return 0;
        out[o++] = (uint8_t)(run - 1);
        memcpy(out + o, in + i, run);
        o += run;
        i += run;
    }
    return o;
}

static bool rle_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                       size_t *out_len) {
    size_t o = 0, i = 0;
    while (i < n) {
        uint8_t tag = in[i++];
        if (tag < 0x80u) {
            size_t run = (size_t)tag + 1;
            if (i + run > n || o + run > cap) return false;
            memcpy(out + o, in + i, run);
            o += run;
            i += run;
        } else {
            size_t run = (size_t)(tag - 0x80u) + 1;
            if (o + run > cap) return false;
            memset(out + o, 0, run);
            o += run;
        }
    }
    *out_len = o;
    return true;
}

// ---- base32(RFC4648 字母表,无填充,MSB-first) ----
static size_t b32_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            if (o + 1 > cap) return 0;
            out[o++] = DR_CODE_B32[(acc >> (bits - 5)) & 0x1Fu];
            bits -= 5;
        }
    }
    if (bits > 0) {                                // 尾部残余位补零成符
        if (o + 1 > cap) return 0;
        out[o++] = DR_CODE_B32[(acc << (5 - bits)) & 0x1Fu];
    }
    return o;
}

static int b32_val(char c) {
    const char *p = strchr(DR_CODE_B32, c);
    return p ? (int)(p - DR_CODE_B32) : -1;
}

// 解前 min(len*5/8, cap) 字节;字符含非法符号返回 false。
static bool b32_decode(const char *in, size_t len, uint8_t *out, size_t cap,
                       size_t *out_len) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b32_val(in[i]);
        if (v < 0) return false;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (o >= cap) return false;
            out[o++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    *out_len = o;
    return true;
}

// ---- 帧打包:核心档 → RLE → 帧 ----
static size_t frame_pack(const dr_code_core_t *core, uint8_t *out, size_t cap) {
    uint8_t payload[DR_CODE_PAYLOAD_MAX];
    size_t plen = rle_pack((const uint8_t *)core, sizeof(*core), payload,
                           sizeof(payload));
    if (plen == 0 || plen > DR_CODE_PAYLOAD_MAX) return 0;
    if (cap < DR_CODE_HDR_SIZE + plen) return 0;
    out[0] = DR_CODE_MAGIC_0;
    out[1] = DR_CODE_MAGIC_1;
    out[2] = (uint8_t)DR_SAVE_VERSION;
    out[3] = (uint8_t)(plen & 0xFFu);
    out[4] = (uint8_t)(plen >> 8);
    memcpy(out + DR_CODE_HDR_SIZE, payload, plen);
    out[5] = crc8_head(out, payload, plen);   // 帧头前 5B + 载荷
    return DR_CODE_HDR_SIZE + plen;
}

size_t dr_savecode_export(const dr_game_t *g, char *out, size_t outsz) {
    dr_code_core_t core;
    memset(&core, 0, sizeof(core));
    memcpy(core.res, g->res, sizeof(core.res));
    core.population = g->population;
    memcpy(core.job, g->job, sizeof(core.job));
    memcpy(core.building_lv, g->building_lv, sizeof(core.building_lv));
    core.flags = g->flags;
    core.weapon_lv = g->weapon_lv;
    core.armor_lv = g->armor_lv;
    core.fire_lv = g->fire_lv;
    core.temp_lv = g->temp_lv;
    core.builder_lv = g->builder_lv;
    core.prestige_count = g->prestige_count;

    uint8_t frame[DR_CODE_HDR_SIZE + DR_CODE_PAYLOAD_MAX];
    size_t flen = frame_pack(&core, frame, sizeof(frame));
    if (flen == 0) return 0;
    return b32_encode(frame, flen, out, outsz ? outsz - 1 : 0);
}

size_t dr_savecode_expect_len(const char *code, size_t len) {
    if (len < 8) return 0;
    uint8_t head[8];
    size_t n;
    if (!b32_decode(code, 8, head, sizeof(head), &n) || n < 5) return 0;
    if (head[0] != DR_CODE_MAGIC_0 || head[1] != DR_CODE_MAGIC_1) return 0;
    if (head[2] != (uint8_t)DR_SAVE_VERSION) return 0;
    size_t plen = (size_t)head[3] | ((size_t)head[4] << 8);
    if (plen == 0 || plen > DR_CODE_PAYLOAD_MAX) return 0;
    size_t total_bytes = DR_CODE_HDR_SIZE + plen;
    return (total_bytes * 8u + 4u) / 5u;           // ceil(bytes*8/5)
}

bool dr_savecode_import(const char *code, size_t len, dr_code_core_t *out) {
    uint8_t frame[DR_CODE_HDR_SIZE + DR_CODE_PAYLOAD_MAX];
    size_t flen;
    if (!b32_decode(code, len, frame, sizeof(frame), &flen)) return false;
    if (flen < DR_CODE_HDR_SIZE) return false;
    if (frame[0] != DR_CODE_MAGIC_0 || frame[1] != DR_CODE_MAGIC_1) return false;
    if (frame[2] != (uint8_t)DR_SAVE_VERSION) return false;
    size_t plen = (size_t)frame[3] | ((size_t)frame[4] << 8);
    if (plen == 0 || plen > DR_CODE_PAYLOAD_MAX) return false;
    if (flen < DR_CODE_HDR_SIZE + plen) return false;   // 码被截断
    if (frame[5] != crc8_head(frame, frame + DR_CODE_HDR_SIZE, plen))
        return false;

    uint8_t core_buf[sizeof(dr_code_core_t)];
    size_t core_len;
    if (!rle_unpack(frame + DR_CODE_HDR_SIZE, plen, core_buf, sizeof(core_buf),
                    &core_len))
        return false;
    if (core_len != sizeof(dr_code_core_t)) return false;
    memcpy(out, core_buf, sizeof(*out));
    return true;
}

void dr_savecode_apply(const dr_code_core_t *core, dr_game_t *g,
                       uint32_t seed, uint32_t now_ts) {
    memset(g, 0, sizeof(*g));
    g->saved_at_ts = now_ts;                       // 不触发离线结算
    memcpy(g->res, core->res, sizeof(g->res));
    for (int i = 0; i < DR_RES_KIND_COUNT; i++)    // 累计口径取下界
        g->res_total[i] = core->res[i];
    g->population = core->population;
    memcpy(g->job, core->job, sizeof(g->job));
    memcpy(g->building_lv, core->building_lv, sizeof(g->building_lv));
    g->flags = core->flags;
    g->rng_seed_state = (seed == 0u) ? 0xDEADBEEFu : seed;
    g->map_seed = (uint16_t)(seed >> 16);          // 新世界(dr_game_init 同派生)
    g->hero_hp = g->hero_hp_max = 10;
    g->weapon_lv = core->weapon_lv;
    g->armor_lv = core->armor_lv;
    g->fire_lv = core->fire_lv;
    g->temp_lv = core->temp_lv;
    g->builder_lv = core->builder_lv;
    g->ship_thrusters = 1;                         // 原版 BASE_THRUSTERS
    g->prestige_count = core->prestige_count;
}
