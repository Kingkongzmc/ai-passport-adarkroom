// main/game_darkroom/dr_state.h —— 存档结构:版本号 + CRC + 迁移。
// 结构体是"内存态"本体,序列化 = 原样字节 + 头部 CRC;NVS 读写由设备适配层完成,
// 本模块只负责:生成镜像 / 校验镜像 / 跨版本迁移 / 离线时间戳兜底。
// 纯 C99,主机可测。约定见 docs/GAMEPLAY.zh_CN.md §6 与 DESIGN.zh_CN.md §0。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dr_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

// 镜像头:魔数 + 版本 + 载荷 CRC。载荷 = 紧随其后的 dr_game_t 原始字节。
typedef struct {
    uint32_t magic;    // 'D','R','S','1' = 0x31535244 (little-endian)
    uint16_t version;  // == DR_SAVE_VERSION
    uint16_t _pad;
    uint32_t body_crc; // dr_crc32(body, sizeof(dr_game_t))
} dr_save_hdr_t;

#pragma pack(pop)

// 运行态游戏数据(持久化的全部内容)。
// 注意保持字段定宽(uint32/uint16/uint8 + 位域),不要用 bool/enum 直存以外的新类型;
// 新字段只允许尾插,历史布局冻结在 dr_state.c 的 dr_game_v1_t 等副本里,
// 由 dr_state_load 逐版本迁移。
typedef struct {
    // 时间:Unix 时间戳(秒)。进入 deep sleep 前写入;唤醒后 Δt = now - ts。
    uint32_t saved_at_ts;

    // 经济:资源存量/累计获得(累计口径:消费不降低收益系数)。
    uint32_t res[DR_RES_KIND_COUNT];
    uint32_t res_total[DR_RES_KIND_COUNT];

    // 村庄:人口与职业分配(job[i] 为该职业在岗人数)。
    uint16_t population;
    uint16_t job[DR_BUILDING_KINDS >= 16 ? 16 : DR_BUILDING_KINDS]; // 职业数上限 16

    // 建筑:等级 0=未建,>0 为已建/等级。
    uint8_t building_lv[DR_BUILDING_KINDS];

    // 剧情:事件已触发次数(0=未触发,冷却/一次性判定用)、标记位。
    uint16_t event_count[128];          // 事件 id 上限 128(M6 灌装 50~70 个)
    uint64_t flags;                     // 剧情标记 DR_FLAG_BITS=64
    uint32_t rng_seed_state;            // 保存 RNG 状态,保证概率序列可复现

    // 小屋:火焰档位(dr_fire_t)。计时基准在运行时,离线视为熄灭。
    uint8_t  fire_lv;

    // 远征(M4 起使用;M1 先占位定宽)。
    uint16_t map_seed;                  // 31×31 随机地点种子(种入存档保证复现)
    uint8_t  hero_x, hero_y;            // 位置(远征中)
    uint8_t  hero_hp, hero_hp_max;
    uint8_t  in_wilderness;             // 0=在小屋,1=远征中
    uint8_t  water, food;               // 远征携带水/食

    // 装备与偏好(v2 尾插;迁移见 dr_state_pack_v1/dr_state_load)。
    uint8_t  armor_lv;                  // 护甲:0=无,1=皮甲(战斗减免 M4 接入)
    uint8_t  trap_bait_on;              // 弃用(v3:饵随查看自动消耗,原版无开关)
    uint8_t  _rsv[2];                   // [0]=猎人半率相位(落盘,原版收入小数累积同义);[1] 预留

    // v3 尾插(对齐原版 room.js 的温度与建造者剧情)。
    uint8_t  temp_lv;                   // 室温 0..4(冻结/冷/微温/暖/热),向火焰档靠拢
    uint8_t  builder_lv;                // 建造者(陌生人)0=无 1=晕倒 2=发抖 3=沉睡 4=帮忙
} dr_game_t;

#pragma pack(push, 1)
typedef struct {
    dr_save_hdr_t hdr;
    dr_game_t     body;
} dr_save_image_t;
#pragma pack(pop)

// 初始化新档(seed 决定随机地图与概率序列)。
void dr_game_init(dr_game_t *g, uint32_t seed, uint32_t now_ts);

// 序列化:g → 镜像(填充头与 CRC)。
void dr_state_pack(const dr_game_t *g, dr_save_image_t *img);

// 反序列化:校验魔数/版本/CRC(仅接受当前版本、当前尺寸的镜像)。
// 历史(更短)镜像走 dr_state_load;返回 false 表示镜像损坏或版本不符。
// 成功时 *out = 镜像中的 body。
bool dr_state_unpack(const dr_save_image_t *img, dr_game_t *out,
                     uint16_t *out_version);

// 读档总入口(设备/主机通用):blob/len 为 NVS 里的原始字节,长度以实际存档
// 为准——旧版本固件写的镜像更短,按“内容长度”验 CRC 后逐版本迁移到当前
// dr_game_t。返回 false = 损坏/未知版本。升级固件不丢档靠这条路径。
bool dr_state_load(const void *blob, size_t len, dr_game_t *out);

// 主机测试用:把 g 压成历史布局镜像(验证迁移链)。返回写入字节数;缓冲不足返回 0。
size_t dr_state_pack_v1(const dr_game_t *g, void *out, size_t outsz);
size_t dr_state_pack_v2(const dr_game_t *g, void *out, size_t outsz);

// 离线结算(M2 起接产出表;M1 提供时间钳制口径):
//   delta_s = now_ts - g->saved_at_ts
//   全速时长 = min(delta_s, DR_OFFLINE_CAP_S);超出部分按 DR_OFFLINE_OVER_PERMILLE 折算。
// 返回折算后的"等效经济 tick 数",并更新 saved_at_ts。
uint32_t dr_offline_ticks(dr_game_t *g, uint32_t now_ts);

#ifdef __cplusplus
}
#endif
