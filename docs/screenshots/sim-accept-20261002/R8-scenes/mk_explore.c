// 场景补拍用"远征者档":只备远征相关能力,不涉及村庄经济。
// 木300 干肉库60 药3 骨矛 皮甲 水袋 森林已开 猎屋1(职业调节 hint 用)
// 人口4 builder4 火3 温3 map_seed=15 saved_at=1770000000。
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
    g.res[DR_RES_WOOD] = 300;
    g.res[DR_RES_FOOD] = 60;      // 干肉库
    g.res[DR_RES_MEDICINE] = 3;
    for (int i = 0; i < DR_RES_KIND_COUNT; i++) g.res_total[i] = g.res[i];

    g.population = 4;
    g.building_lv[DR_BLD_LODGE] = 1;   // 解锁猎人(调节 hint 场景)

    g.flags = 0;
    uint64_t fs[] = { DR_FLAG_FOREST, DR_FLAG_WATERSKIN };
    for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++)
        g.flags |= (uint64_t)1u << fs[i];
    g.rng_seed_state = 0xBEEF1234u;

    g.fire_lv = 3;   g.temp_lv = 3;   g.builder_lv = 4;
    g.weapon_lv = 1; g.armor_lv = 1;  // 骨矛+皮甲 → HP15,危险阈值 r8
    g.map_seed = 15;
    g.hero_hp = g.hero_hp_max = 15;
    g.water = 20;    g.food = 0;

    dr_save_image_t img;
    dr_state_pack(&g, &img);
    FILE *f = fopen("sim_dr_save.bin", "wb");
    fwrite(&img, 1, sizeof(img), f);
    fclose(f);
    printf("explore save written: %zu bytes\n", sizeof(img));
    return 0;
}
