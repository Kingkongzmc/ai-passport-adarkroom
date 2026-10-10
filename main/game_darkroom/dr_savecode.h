// main/game_darkroom/dr_savecode.h —— 存档码:核心档 → RLE → 帧 → base32。
// 背景:官方社区浏览器烧录是"单一合并镜像 0x0 全量写入",必然覆盖 NVS 存档区
// (IMPLEMENTATION_STATUS §6);存档码是对该渠道的唯一保档手段。
// 纯 C99,主机可测。格式(拍板,DESIGN §6):
//   帧 = 'D''K' + 版本(1B,DR_SAVE_VERSION) + 载荷长(2B LE) + CRC8(1B) + 载荷
//   载荷 = 核心档(dr_code_core_t,定长 173B)经零游程 RLE 压缩后的字节
//   CRC8(poly 0x07,init 0)覆盖帧头前 5 字节 + 载荷
//   文本 = 整帧 base32(A-Z2-7,无填充),UI 按 6 字符/页分页
// 核心档字段 = 拍板清单(res/population/jobs/building_lv/flags/weapon/armor/
// prestige)+ fire/temp/builder 三个剧情字节(防止恢复档进入"森林已解锁而
// 建造者剧情未走"的矛盾态,3 字节 RLE 后 ~4 字符);丢弃 event_count/map_seed
// 导入时重新生成。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dr_config.h"
#include "dr_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// 帧头:'DK' + ver + len(2B LE) + crc8。
#define DR_CODE_HDR_SIZE 6
// base32 字母表(全在 f12 闭集 ASCII range 内)。
#define DR_CODE_B32 "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
// RLE 最坏输出 = n + ceil(n/128);帧最长 = 6 + (174+2) = 182B
// → base32 上限 = ceil(182*8/5) = 292 字符。UI 缓冲按 320 给。
#define DR_CODE_MAX_CHARS 320u

#pragma pack(push, 1)

// 核心档:拍板字段清单,定长打包(sizeof=173,布局冻结——改字段 = 码不兼容,
// 版本字节由 DR_SAVE_VERSION 承担,升版后老码拒绝导入)。
typedef struct {
    uint32_t res[DR_RES_KIND_COUNT];          // 84B
    uint16_t population;                      // 2B
    uint16_t job[16];                         // 32B(与 dr_game_t.job 同维)
    uint8_t  building_lv[DR_BUILDING_KINDS];  // 40B
    uint64_t flags;                           // 8B
    uint8_t  weapon_lv, armor_lv;             // 2B
    uint8_t  fire_lv, temp_lv, builder_lv;    // 3B(防矛盾态,见文件头)
    uint16_t prestige_count;                  // 2B
} dr_code_core_t;

#pragma pack(pop)

_Static_assert(sizeof(dr_code_core_t) == 173u, "core layout is frozen at 173B");

// 导出:g → 帧 → base32 文本。返回写入字符数(不含 NUL);outsz 不足返回 0。
// 调用方缓冲 ≥ DR_CODE_MAX_CHARS+1。
size_t dr_savecode_export(const dr_game_t *g, char *out, size_t outsz);

// 预读总字符数:从已输入的前 8 个字符解出帧头(魔数/版本须合法),返回完整码
// 的 base32 总长;帧头不合法或输入不足 8 字符返回 0(调用方以此判"格式错误")。
size_t dr_savecode_expect_len(const char *code, size_t len);

// 导入:base32 文本(长度须 == expect_len)→ 校验 CRC8 → RLE 解压(长度必须
// 恰为核心档定长)。任一环失败返回 false。
bool dr_savecode_import(const char *code, size_t len, dr_code_core_t *out);

// 核心档 → 完整游戏态。丢弃字段的口径:saved_at_ts=now(不触发离线结算)、
// map_seed/rng_seed_state 由 seed 派生(新世界)、event_count 清零(事件重走)、
// in_wilderness=0(远征态不保,世界已换)、hero/水粮初始、res_total 取当前
// res(累计口径下界)、ship_thrusters=1(原版 BASE_THRUSTERS;星舰硬件数值
// 不在拍板清单,恢复档按初始)。
void dr_savecode_apply(const dr_code_core_t *core, dr_game_t *g,
                       uint32_t seed, uint32_t now_ts);

#ifdef __cplusplus
}
#endif
