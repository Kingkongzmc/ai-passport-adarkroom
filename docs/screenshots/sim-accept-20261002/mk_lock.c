// 验收专用造档工具(补修2 复现档):森林开 + 全职业锁。
// 规格:建筑全 0、旗标仅 FOREST、人口 4、builder_lv=4(勿用 0,避免触发森林解锁剧情把木置 4)、
// 火 3 温 3、map_seed=1234、saved_at=1770000000。
// 只生成 sim_dr_save.bin(无日志档:职业窗区断言不依赖日志),不触碰项目源码。
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dr_config.h"
#include "dr_rules.h"
#include "dr_state.h"
#include "dr_util.h"

int main(void) {
    dr_game_t g;
    memset(&g, 0, sizeof(g));
    g.saved_at_ts = 1770000000u;

    g.res[DR_RES_WOOD] = 500;         // 村庄页不消耗,任意
    g.res[DR_RES_FUR] = 10;
    g.res[DR_RES_MEAT] = 10;
    for (int i = 0; i < DR_RES_KIND_COUNT; i++) g.res_total[i] = g.res[i];

    g.population = 4;                 // 全闲人(采集者 4 人)
    for (int i = 0; i < DR_JOB_KIND_COUNT; i++) g.job[i] = 0;

    for (int i = 0; i < DR_BLD_KIND_COUNT; i++) g.building_lv[i] = 0;  // 全 0 → 全职业锁

    g.flags = (uint64_t)1u << DR_FLAG_FOREST;   // 仅森林 → 村庄页签开
    g.rng_seed_state = 0xBEEF1234u;

    g.fire_lv  = 3;
    g.temp_lv  = 3;
    g.builder_lv = 4;
    g.weapon_lv = 0;
    g.armor_lv  = 0;
    g.map_seed = 1234;
    g.hero_hp = g.hero_hp_max = 10;
    g.water = 10;
    g.food  = 0;

    dr_save_image_t img;
    dr_state_pack(&g, &img);
    FILE *f = fopen("sim_dr_save.bin", "wb");
    fwrite(&img, 1, sizeof(img), f);
    fclose(f);
    printf("lock save written: %zu bytes\n", sizeof(img));
    return 0;
}
