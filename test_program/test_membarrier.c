/*
 * test_membarrier.c - membarrier(2) 应使用硬件内存屏障
 *
 * 对应 bug: StarryOS membarrier 使用 compiler_fence 而非 atomic::fence
 * 对应 PR:  https://github.com/rcore-os/tgoskits/pull/201
 *
 * 编译: riscv64-linux-musl-gcc -static -o test_membarrier test_membarrier.c
 */

#include "test_framework.h"
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_membarrier
#if defined(__riscv)
#define __NR_membarrier 283
#elif defined(__x86_64__)
#define __NR_membarrier 324
#elif defined(__aarch64__)
#define __NR_membarrier 283
#elif defined(__loongarch__)
#define __NR_membarrier 283
#endif
#endif

#define MEMBARRIER_CMD_QUERY 0
#define MEMBARRIER_CMD_GLOBAL 1

int main(void) {
    TEST_START("membarrier: atomic fence correctness");

    /* QUERY 应返回支持的命令掩码（非负） */
    long r = syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    CHECK(r >= 0, "QUERY returns non-negative bitmask");

    /* GLOBAL 屏障应成功（需要真正的硬件 fence） */
    r = syscall(__NR_membarrier, MEMBARRIER_CMD_GLOBAL, 0, 0);
    CHECK(r == 0, "GLOBAL barrier succeeds");

    /* 非法 cmd 应返回 EINVAL */
    CHECK_ERR(syscall(__NR_membarrier, 0x80000000, 0, 0), EINVAL,
              "invalid cmd returns EINVAL");

    /* flags != 0 应返回 EINVAL */
    CHECK_ERR(syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 1, 0), EINVAL,
              "non-zero flags returns EINVAL");

    TEST_DONE();
}
