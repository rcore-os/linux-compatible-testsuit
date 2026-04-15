/*
 * test_getgroups.c -- getgroups 系统调用测试
 *
 * 测试内容：
 *   1. size=0 时应返回附加组数量，不写入缓冲区
 *   2. 正常获取附加组列表，数量与 size=0 查询一致
 */

#define _GNU_SOURCE
#include "test_framework.h"
#include <unistd.h>
#include <sys/types.h>

int main(void) {
    TEST_START("getgroups");

    /* size=0 应返回附加组数量（>= 0），不写入缓冲区 */
    int count = getgroups(0, NULL);
    CHECK(count >= 0, "size=0 返回非负组数量");

    /* 缓冲区足够大时正常获取，数量应与 size=0 查询一致 */
    {
        gid_t groups[256];
        int n = getgroups(256, groups);
        CHECK(n >= 0, "正常获取附加组");
        CHECK(n == count, "实际获取数量与 size=0 查询一致");
    }

    TEST_DONE();
}
