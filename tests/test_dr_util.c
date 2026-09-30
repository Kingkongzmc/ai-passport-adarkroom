// tests/test_dr_util.c —— dr_fmt_compact 主机测试:档位边界 + 输出宽度上界。
// 资源格宽 46px(f12:数字 6.69px/汉字 12px),数字部分最长 "99.9k"≈30px,
// "木 99.9k" = 44.7px 不被 LONG_DOT 截断(见 docs/UI_FIX_PLAN.zh_CN.md I1)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_util.h"

static void expect(uint32_t v, const char *want) {
    char buf[16];
    dr_fmt_compact(buf, sizeof(buf), v);
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "dr_fmt_compact(%u) = \"%s\", want \"%s\"\n",
                (unsigned)v, buf, want);
        assert(false);
    }
    assert(strlen(buf) <= 5);   // 最长 "10.0k"/"99.9k" ≈30px,"木 99.9k"=44.7px ≤46px
}

static void test_compact_tiers(void) {
    expect(0, "0");
    expect(9, "9");
    expect(123, "123");
    expect(9999, "9999");        // 4 位原样(41.4px 放得下)
    expect(10000, "10.0k");      // 5 位起进 k 档
    expect(12345, "12.3k");      // +50 四舍五入(12395/1000=12.3…舍)
    expect(99499, "99.5k");      // 一位小数档上限
    expect(99500, "100k");       // 四舍五入溢出两位整数 → 整 k 档
    expect(99999, "100k");
    expect(100000, "100k");
    expect(123456, "123k");
    expect(999499, "999k");      // 整 k 档上限
    expect(999500, "1.0M");      // 进 M 档
    expect(1200000, "1.2M");
    expect(99949999u, "99.9M");
    expect(99950000u, "100M");   // 两位整数 M,资源量到不了,仅保证不崩
}

int main(void) {
    test_compact_tiers();
    printf("test_dr_util passed\n");
    return 0;
}
