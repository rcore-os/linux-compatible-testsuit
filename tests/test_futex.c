/*
 * test_futex.c — futex 用户态同步原语测试
 *
 * 测试策略：通过 fork + 共享内存验证 FUTEX_WAIT/WAKE/PRIVATE/TIMEOUT 语义
 *
 * 覆盖范围：
 *   正向：FUTEX_WAIT/WAKE 基本、超时、值不匹配、无等待者 WAKE、
 *         多等待者唤醒、FUTEX_PRIVATE、fork 跨进程
 *   负向：无效指针 EFAULT
 */

#include "test_framework.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/futex.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define TMPFILE "/tmp/starry_futex_test"

static inline int futex_wait(int *uaddr, int val, const struct timespec *timeout)
{
    return (int)syscall(SYS_futex, uaddr, FUTEX_WAIT, val, timeout, NULL, 0);
}

static inline int futex_wake(int *uaddr, int count)
{
    return (int)syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

static inline int futex_wait_private(int *uaddr, int val, const struct timespec *timeout)
{
    return (int)syscall(SYS_futex, uaddr, FUTEX_WAIT_PRIVATE, val, timeout, NULL, 0);
}

static inline int futex_wake_private(int *uaddr, int count)
{
    return (int)syscall(SYS_futex, uaddr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

int main(void)
{
    TEST_START("futex: FUTEX_WAIT/WAKE/PRIVATE/TIMEOUT");

    unlink(TMPFILE);

    /* ================================================================
     * PART 1: 基本 FUTEX_WAIT + FUTEX_WAKE
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED, "PART1: mmap 共享内存成功");
        if (fut == MAP_FAILED) { TEST_DONE(); }

        *fut = 1;

        pid_t pid = fork();
        if (pid == 0) {
            usleep(50000);
            *fut = 2;
            futex_wake(fut, 1);
            _exit(0);
        }

        futex_wait(fut, 1, NULL);
        CHECK(*fut == 2,
              "PART1: wait 被唤醒后 futex 值 == 2");

        int status;
        wait4(pid, &status, 0, NULL);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "PART1: 子进程正常退出");

        munmap(fut, sizeof(int));
    }

    /* ================================================================
     * PART 2: FUTEX_WAIT 超时 → ETIMEDOUT
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED, "PART2: mmap 共享内存成功");
        if (fut == MAP_FAILED) { TEST_DONE(); }

        *fut = 42;

        struct timespec ts = {0, 50000000L};

        errno = 0;
        int ret = futex_wait(fut, 42, &ts);
        CHECK(ret == -1 && errno == ETIMEDOUT,
              "PART2: FUTEX_WAIT 超时返回 ETIMEDOUT");

        munmap(fut, sizeof(int));
    }

    /* ================================================================
     * PART 3: FUTEX_WAIT 值不匹配 → EWOULDBLOCK
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED, "PART3: mmap 共享内存成功");
        if (fut == MAP_FAILED) { TEST_DONE(); }

        *fut = 10;

        errno = 0;
        int ret = futex_wait(fut, 99, NULL);
        CHECK(ret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN),
              "PART3: 值不匹配返回 EWOULDBLOCK/EAGAIN");

        munmap(fut, sizeof(int));
    }

    /* ================================================================
     * PART 4: 无等待者 FUTEX_WAKE → 返回 0
     * ================================================================ */

    {
        int val = 1;
        int woken = futex_wake(&val, 1);
        CHECK_RET(woken, 0, "PART4: 无等待者 WAKE 返回 0");
    }

    /* ================================================================
     * PART 5: 多等待者 WAKE(N)
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int *woken_flag = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED && woken_flag != MAP_FAILED,
              "PART5: mmap 共享内存成功");
        if (fut == MAP_FAILED || woken_flag == MAP_FAILED) { TEST_DONE(); }

        *fut = 1;
        *woken_flag = 0;

        int n_children = 3;
        pid_t pids[3];
        for (int i = 0; i < n_children; i++) {
            pids[i] = fork();
            if (pids[i] == 0) {
                futex_wait(fut, 1, NULL);
                __sync_fetch_and_add(woken_flag, 1);
                _exit(0);
            }
        }

        usleep(100000);

        int woken = futex_wake(fut, 2);
        CHECK(woken >= 1 && woken <= 2,
              "PART5: WAKE(2) 唤醒 1~2 个等待者");

        usleep(100000);

        CHECK(*woken_flag >= 1 && *woken_flag <= 2,
              "PART5: 实际被唤醒的子进程数 1~2");

        futex_wake(fut, n_children);
        for (int i = 0; i < n_children; i++) {
            wait4(pids[i], NULL, 0, NULL);
        }

        munmap(fut, sizeof(int));
        munmap(woken_flag, sizeof(int));
    }

    /* ================================================================
     * PART 6: FUTEX_WAIT_PRIVATE / FUTEX_WAKE_PRIVATE
     * ================================================================ */

    {
        int val = 0;
        errno = 0;
        int woken = futex_wake_private(&val, 1);
        CHECK_RET(woken, 0, "PART6: PRIVATE WAKE 无等待者返回 0");

        errno = 0;
        int ret = futex_wait_private(&val, 99, NULL);
        CHECK(ret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN),
              "PART6: PRIVATE WAIT 值不匹配返回 EWOULDBLOCK");
    }

    /* ================================================================
     * PART 7: fork 后跨进程 wait/wake
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED, "PART7: mmap 共享内存成功");
        if (fut == MAP_FAILED) { TEST_DONE(); }

        *fut = 0;

        pid_t pid = fork();
        if (pid == 0) {
            *fut = 100;
            futex_wake(fut, 1);
            _exit(0);
        }

        futex_wait(fut, 0, NULL);
        CHECK(*fut == 100,
              "PART7: 跨进程 wait 后 futex 值被子进程更新为 100");

        wait4(pid, NULL, 0, NULL);
        munmap(fut, sizeof(int));
    }

    /* ================================================================
     * PART 8: 负向测试
     * ================================================================ */

    {
        errno = 0;
        int ret = futex_wait(NULL, 0, NULL);
        CHECK(ret == -1 && (errno == EFAULT || errno == EINVAL),
              "PART8: WAIT NULL 指针返回 EFAULT/EINVAL");
    }

    /* ================================================================
     * PART 9: 超时精度 — 短超时不应立刻返回
     * ================================================================ */

    {
        int *fut = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fut != MAP_FAILED, "PART9: mmap 共享内存成功");
        if (fut == MAP_FAILED) { TEST_DONE(); }

        *fut = 1;

        struct timespec ts = {0, 100000000L};

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        errno = 0;
        futex_wait(fut, 1, &ts);

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000L +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000L;
        CHECK(elapsed_ms >= 50,
              "PART9: 超时等待时间 >= 50ms (实际可能更长)");

        munmap(fut, sizeof(int));
    }

    unlink(TMPFILE);

    TEST_DONE();
}
