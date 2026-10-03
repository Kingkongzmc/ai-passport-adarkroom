// sim_shots.c —— 场景补拍驱动:复用 sim_play.c 全部设施(同编译单元
// #include,main 重命名弃用),按环境变量分两段:
//   段一(默认): 新档前期态(太冷建造/点火冷却/森林置木/回暖/导航焦点/
//               贸易无站/火熄缺木)
//   段二(SHOTS2=1 + SIM_PERSIST + 远征者档): 村庄调节 hint、三种地点页
//               (老屋/废镇/城市)+地点搜索+战斗逃跑+危险态(里红字)+
//               吃干肉+渴死+120s 出发冷却
#define main sim_play_main_disabled
#include "sim_play.c"
#undef main

static void flee_fight(void) {
    for (int t = 0; t < 6 && page_is(PG_COMBATX); t++) {
        shot("e-combat");
        P = PG_COMBATX; F = -1;
        CHECK(focus_to(3, combat_en, combat_lines()), "逃跑行找不到");
        k_ok_saved(); snap();
        printf("[%7.1fs]   逃跑尝试%d xy=(%u,%u) hp=%u\n",
               vt_s(), t, G.hero_x, G.hero_y, G.hero_hp);
    }
    shot("e-fled");
}
static void walk_step(int dir) {   // 0东 1南 2西 3北
    if (page_is(PG_COMBATX)) flee_fight();
    if (page_is(PG_RUINX)) return;   // 到达地点,交调用方处理
    P = PG_MAP_EXPX; F = -1;
    CHECK(page_is(PG_MAP_EXPX), "远征态指纹不符");
    CHECK(focus_to(dir, map_exp_en, map_exp_lines()), "方向行 %d 找不到", dir);
    k_ok_saved(); snap();
    printf("[%7.1fs]   走 dir=%d xy=(%u,%u) water=%u food=%u wild=%u\n",
           vt_s(), dir, G.hero_x, G.hero_y, G.water, G.food, G.in_wilderness);
}
// 走到地点/战斗/死亡为止;返回时若在地点页返回 true
static bool walk_until(const int *path, int n) {
    for (int i = 0; i < n; i++) {
        if (!G.in_wilderness) return false;
        if (page_is(PG_RUINX)) return true;
        walk_step(path[i]);
        if (page_is(PG_RUINX)) return true;
    }
    return page_is(PG_RUINX);
}
static void embark(void) {
    for (int i = 0; i < 12; i++) {          // 轮播计数器跨段保留:循环到整备页
        sweep_n();
        if (page_is(PG_MAP_OUTFITX)) break;
    }
    P = PG_MAP_OUTFITX; F = 0;
    CHECK(page_is(PG_MAP_OUTFITX), "未落到整备页");
    k_ok();
    for (int i = 0; i < 15; i++) k_up();   // 水袋容量 10,后 5 次被"背袋满"拦下
    k_ok(); k_ok(); k_ok();
    snap();
    CHECK(G.food == 10, "干肉未带满袋 (%u)", G.food);
    CHECK(focus_to(5, map_outfit_en, map_outfit_lines()), "出发行找不到");
    k_ok_saved(); snap();
    CHECK(G.in_wilderness == 1, "未进远征态");
}
static void ruin_search(void) {
    P = PG_RUINX; F = -1;
    CHECK(page_is(PG_RUINX), "地点页指纹不符");
    CHECK(focus_to(0, combat_en, 2), "搜索行找不到");
    k_ok_saved(); snap();
    shot("e-ruin-search");
    if (page_is(PG_COMBATX)) flee_fight();   // 搜索触发遭遇
}
static void ruin_leave(void) {
    P = PG_RUINX; F = -1;
    CHECK(focus_to(1, combat_en, 2), "离开行找不到");
    k_ok_saved(); snap();
}
static int cell3_red(void) {   // "里"格红字(危险态)
    refresh_fb();
    int red = 0;
    for (int y = 45; y <= 59; y++)
        for (int x = 168; x <= 213; x++) {
            const uint8_t *p = s_fb + ((size_t)y * SIM_W + x) * 4;
            if (p[2] > 180 && p[1] < 90 && p[0] < 90) red++;
        }
    return red;
}

static void run_stage1(void) {
    darkroom_app_enter();
    frames(60);
    P = PG_TITLEX;
    snap();
    shot("s01-title-newgame");
    k_ok(); P = PG_HOMEX; F = 0; snap();
    shot("s02-home-newgame");
    focus_to(1, home_en, home_lines());
    k_ok(); P = PG_BUILDX; F = 0;
    if (!build_en(0)) sim_move(build_en, build_lines(), 1);
    snap(); shot("s03-build-too-cold");     // 冻结态提示(火未点)
    k_long(); P = PG_HOMEX; F = 0; snap();
    focus_to(0, home_en, home_lines());
    k_ok_saved(); snap();
    CHECK(G.fire_lv == DR_FIRE_BURNING, "点火失败");
    shot("s04-home-stoking");               // 点火中…(冷却填充条)
    for (int i = 0; i < 6 && !(G.flags & ((uint64_t)1u << DR_FLAG_FOREST)); i++)
        park_wait(10.0);
    CHECK(G.flags & ((uint64_t)1u << DR_FLAG_FOREST), "森林未解锁");
    snap(); shot("s05-forest-wood4");       // 木被置 4
    enter_build(); snap(); shot("s06-build-warming");
    enter_home();
    k_up();
    snap(); shot("s07-home-nav-focus");     // 页签导航焦点(金框)
    k_down(); F = 0; snap();
    for (int i = 0; i < 8; i++) sweep_n();  // 轮播到贸易页
    P = PG_TRADEX; F = 0;
    snap(); shot("s08-trade-need-post");    // 8 行"需贸易站"灰显
    k_long(); P = PG_HOMEX; F = 0; snap();
    for (int i = 0; i < 80 && G.fire_lv > 0; i++) park_wait(60.0);
    CHECK(G.fire_lv == 0, "火未熄 (%u)", G.fire_lv);
    snap(); shot("s09-home-fire-out-needwood");   // 点火(缺木)
    printf("[%7.1fs] == 段一完成 ==\n", vt_s());
}

static void run_stage2(void) {
    darkroom_app_enter();
    frames(60);
    fast_wait(61.0);        // 读档路径无立即存档:等首个 60s 自动存档落镜像
    P = PG_TITLEX;
    snap();
    CHECK(G.res[DR_RES_FOOD] == 60 && G.weapon_lv == 1,
          "远征者档未载入(FOOD=%u weapon=%u)", G.res[DR_RES_FOOD], G.weapon_lv);
    shot("s10-title-explore");
    k_ok(); P = PG_HOMEX; F = 0; snap();
    shot("s11-home-explore");

    // ---- 村庄职业调节 hint(猎屋已建) ----
    enter_village();
    CHECK(focus_to(3, village_en, village_lines()), "猎人行找不到");
    k_ok();                                  // 进入调节
    snap(); shot("s12-village-adj-hint");    // "上加 下减(长按=5) 确定=完成"
    k_up(); k_up();
    snap(); shot("s13-village-adj-2");
    k_ok(); snap();                          // 退出调节
    CHECK(G.job[DR_JOB_HUNTER] == 2, "调节未生效 (%u)", G.job[DR_JOB_HUNTER]);
    enter_home(); shot("s14-village-assigned");

    // ---- 远征A:洞穴(无火把变体)→绕行老屋 + 搜索 + 危险态 + 吃干肉 + 回家 ----
    embark(); shot("s20-outfit-loaded"); shot("s21-exp-start");
    { static const int p[3] = {1,1,0};   walk_until(p, 3); }   // 南2东1→洞穴
    CHECK(page_is(PG_RUINX), "未到洞穴");
    shot("s22-ruin-cave");
    ruin_search();                         // 无火把 → "需要火把"提示变体
    ruin_leave();
    { static const int p[6] = {1,0,0,0,0,3}; walk_until(p, 6); } // 南1东4北1→老屋
    CHECK(page_is(PG_RUINX), "未到老屋");
    shot("s23-ruin-house");
    ruin_search();
    ruin_leave();
    walk_step(0);                          // 再东 1 → (36,32) r8
    if (page_is(PG_COMBATX)) flee_fight(); // 该步可能触发遭遇战(逃跑场景)
    refresh_fb();
    CHECK(cell3_red() > 0, "危险态里格未变红");
    shot("s24-danger-red-dist");
    P = PG_MAP_EXPX; F = -1;
    CHECK(focus_to(4, map_exp_en, map_exp_lines()), "吃干肉行找不到");
    k_ok_saved(); snap(); shot("s25-eat-jerky");
    { static const int p[8] = {3,3,2,2,2,2,2,2};  walk_until(p, 8); } // 北2西6(绕开老屋)
    CHECK(!G.in_wilderness, "未到家");
    P = PG_HOMEX; F = 0;
    enter_home(); shot("s26-home-returnA");

    // ---- 远征B:废镇(r10) + 搜索 + 回家 ----
    embark(); shot("s30-exp-start-B");
    { static const int p[10] = {2,2,2,2,2,2,2,3,3,3}; walk_until(p, 10); } // 西7北3
    CHECK(page_is(PG_RUINX), "未到废镇");
    shot("s31-ruin-town");
    ruin_search();
    ruin_leave();
    { static const int p[10] = {0,0,0,0,0,0,0,1,1,1}; walk_until(p, 10); } // 东7南3
    snap(); shot("s32-home-returnB");
    P = PG_HOMEX; F = 0;

    // ---- 远征C:城市(r20) + 渴死 + 120s 冷却 ----
    embark(); shot("s40-exp-start-C");
    { static const int p[20] = {3,3,3,3,3,3,3,2,2,2,2,2,2,2,2,2,2,2,2,2};
      walk_until(p, 20); }                    // 北7西13
    CHECK(page_is(PG_RUINX), "未到城市 water=%u", G.water);
    shot("s41-ruin-city");
    ruin_leave();
    walk_step(1);                             // 水 0 → 口渴警告(日志)
    walk_step(1);                             // 再走 → 渴死
    CHECK(!G.in_wilderness && G.water == 0, "未渴死 water=%u wild=%u",
          G.water, G.in_wilderness);
    snap(); shot("s42-death-outfit");         // 物资全失回整备
    P = PG_MAP_OUTFITX; F = -1;
    CHECK(page_is(PG_MAP_OUTFITX), "整备态指纹不符");
    // 出发行因 food=0 被禁用(焦点不可落),但 v 列"120s"冷却倒计时仍渲染
    CHECK(focus_to(6, map_outfit_en, map_outfit_lines()), "返回行找不到");
    snap(); shot("s43-embark-cooldown-120s");
    k_long(); P = PG_HOMEX; F = 0; snap();
    enter_home(); shot("s44-home-death-log");
    printf("[%7.1fs] == 段二完成 ==\n", vt_s());
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s_out = argc > 1 ? argv[1] : "";
    s_t0_us = s_vtime_us;

    lv_init();
    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    if (getenv("SHOTS2")) run_stage2();
    else run_stage1();
    sim_lvgl_lock(1000);
    dr_debug_dump();
    sim_lvgl_unlock();
    printf("sim_shots done\n");
    return 0;
}
