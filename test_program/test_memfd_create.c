/*
 * test_memfd_create.c — memfd_create 功能完整验证
 *
 * 测试策略：验证匿名文件创建、读写、mmap、sealing、flags 及错误路径
 *
 * 覆盖范围：
 *   正向：基本创建、ftruncate 设大小、读写数据、mmap 映射、
 *         MFD_CLOEXEC 标志、MFD_ALLOW_SEALING 与 sealing 操作、
 *         F_SEAL_SHRINK/GROW/WRITE 生效验证、
 *         /proc/self/fd 符号链接、fork 继承
 *   负向：EFAULT(EINVAL) 无效 name、EINVAL 未知 flags、EINVAL name 过长
 */

#include "test_framework.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

/* sealing 相关常量（部分系统可能未定义） */
#ifndef F_ADD_SEALS
#define F_ADD_SEALS     1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS     1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL     0x0001
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK   0x0002
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW     0x0004
#endif
#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE    0x0008
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC     0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#define TEST_DATA "Hello, memfd_create test!"
#define PAGE_SIZE 4096

int main(void)
{
    TEST_START("memfd_create: 匿名文件创建与 sealing 验证");

    /* ================================================================
     * PART 1: 基本 memfd_create 创建
     * ================================================================ */

    /* 1.1 创建匿名文件 (flags=0) */
    int fd = memfd_create("test_basic", 0);
    CHECK(fd >= 0, "memfd_create 基本创建成功");
    if (fd < 0) { TEST_DONE(); }

    /* 1.2 验证 fd 为 O_RDWR */
    int fl = fcntl(fd, F_GETFL);
    CHECK(fl >= 0, "fcntl F_GETFL 成功");
    CHECK((fl & O_ACCMODE) == O_RDWR, "memfd fd 为 O_RDWR");

    /* 1.3 验证初始大小为 0 */
    struct stat st;
    CHECK_RET(fstat(fd, &st), 0, "fstat 成功");
    CHECK_RET(st.st_size, 0, "memfd 初始大小为 0");

    close(fd);

    /* ================================================================
     * PART 2: ftruncate 设大小 + 读写验证
     * ================================================================ */

    fd = memfd_create("test_rw", 0);
    CHECK(fd >= 0, "memfd_create(rw test) 成功");
    if (fd < 0) { TEST_DONE(); }

    /* 2.1 ftruncate 设置大小 */
    CHECK_RET(ftruncate(fd, PAGE_SIZE), 0, "ftruncate 到 4096 成功");
    CHECK_RET(fstat(fd, &st), 0, "ftruncate 后 fstat");
    CHECK(st.st_size == PAGE_SIZE, "ftruncate 后大小 == 4096");

    /* 2.2 write 数据 */
    const char *msg = TEST_DATA;
    int msg_len = strlen(msg);
    ssize_t wret = write(fd, msg, msg_len);
    CHECK_RET(wret, msg_len, "write 到 memfd 成功");

    /* 2.3 lseek 回头 + read 验证（只读 msg_len 字节以精确匹配） */
    lseek(fd, 0, SEEK_SET);
    char buf[128] = {0};
    ssize_t rret = read(fd, buf, msg_len);
    CHECK(rret == msg_len, "从 memfd 读取到预期字节数");
    CHECK(strcmp(buf, msg) == 0, "memfd 读回内容与写入一致");

    close(fd);

    /* ================================================================
     * PART 3: mmap 映射验证
     * ================================================================ */

    fd = memfd_create("test_mmap", 0);
    CHECK(fd >= 0, "memfd_create(mmap test) 成功");
    if (fd < 0) { TEST_DONE(); }

    CHECK_RET(ftruncate(fd, PAGE_SIZE), 0, "mmap 测试: ftruncate");

    /* 3.1 mmap 写入 */
    char *map = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED, "mmap MAP_SHARED 成功");
    if (map != MAP_FAILED) {
        const char *mmap_msg = "mmap data test";
        memcpy(map, mmap_msg, strlen(mmap_msg));

        /* 3.2 通过 read 验证 mmap 写入的数据 */
        lseek(fd, 0, SEEK_SET);
        char mmap_buf[64] = {0};
        rret = read(fd, mmap_buf, sizeof(mmap_buf) - 1);
        CHECK(rret > 0, "mmap 后通过 read 读回数据");
        CHECK(strncmp(mmap_buf, mmap_msg, strlen(mmap_msg)) == 0,
              "mmap 写入与 read 读回一致");

        munmap(map, PAGE_SIZE);
    }

    close(fd);

    /* ================================================================
     * PART 4: MFD_CLOEXEC 标志
     * ================================================================ */

    fd = memfd_create("test_cloexec", MFD_CLOEXEC);
    CHECK(fd >= 0, "memfd_create MFD_CLOEXEC 成功");
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        CHECK(fd_flags >= 0, "MFD_CLOEXEC: F_GETFD 成功");
        CHECK((fd_flags & FD_CLOEXEC) != 0,
              "MFD_CLOEXEC: FD_CLOEXEC 已设置");
        close(fd);
    }

    /* 无 MFD_CLOEXEC 时不应有 FD_CLOEXEC */
    fd = memfd_create("test_no_cloexec", 0);
    CHECK(fd >= 0, "memfd_create flags=0 成功");
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        CHECK(fd_flags >= 0, "flags=0: F_GETFD 成功");
        CHECK((fd_flags & FD_CLOEXEC) == 0,
              "flags=0: FD_CLOEXEC 未设置");
        close(fd);
    }

    /* ================================================================
     * PART 5: MFD_ALLOW_SEALING 与初始 seal 状态
     * ================================================================ */

    /* 5.1 有 MFD_ALLOW_SEALING → 初始 seal 为空 */
    fd = memfd_create("test_seal", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create MFD_ALLOW_SEALING 成功");
    if (fd >= 0) {
        int seals = fcntl(fd, F_GET_SEALS);
        CHECK(seals >= 0, "MFD_ALLOW_SEALING: F_GET_SEALS 成功");
        CHECK(seals == 0, "MFD_ALLOW_SEALING: 初始 seals 为空");

        /* 5.2 先设置非零大小，再加 F_SEAL_SHRINK */
        ftruncate(fd, PAGE_SIZE);

        CHECK_RET(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK), 0,
                  "F_ADD_SEALS(F_SEAL_SHRINK) 成功");
        seals = fcntl(fd, F_GET_SEALS);
        CHECK((seals & F_SEAL_SHRINK) != 0,
              "F_GET_SEALS 包含 F_SEAL_SHRINK");

        /* 5.3 F_SEAL_SHRINK 生效：ftruncate 缩小应失败 */
        errno = 0;
        int trunc_ret = ftruncate(fd, 0);
        CHECK(trunc_ret == -1 && (errno == EPERM || errno == EINVAL),
              "F_SEAL_SHRINK 后 ftruncate 缩小失败");

        close(fd);
    }

    /* 5.4 无 MFD_ALLOW_SEALING → 初始 seal 为 F_SEAL_SEAL */
    fd = memfd_create("test_no_seal", 0);
    CHECK(fd >= 0, "memfd_create flags=0 (no sealing) 成功");
    if (fd >= 0) {
        int seals = fcntl(fd, F_GET_SEALS);
        CHECK(seals >= 0, "flags=0: F_GET_SEALS 成功");
        CHECK((seals & F_SEAL_SEAL) != 0,
              "flags=0: 初始 seals 包含 F_SEAL_SEAL");

        /* 5.5 尝试添加 seal 应失败 */
        errno = 0;
        CHECK_ERR(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK), EPERM,
                  "F_SEAL_SEAL 状态下 F_ADD_SEALS 返回 EPERM");

        close(fd);
    }

    /* ================================================================
     * PART 6: F_SEAL_GROW 生效验证
     * ================================================================ */

    fd = memfd_create("test_grow_seal", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(grow seal test) 成功");
    if (fd >= 0) {
        ftruncate(fd, PAGE_SIZE);
        CHECK_RET(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW), 0,
                  "F_ADD_SEALS(F_SEAL_GROW) 成功");

        /* ftruncate 增大应失败 */
        errno = 0;
        int grow_ret = ftruncate(fd, PAGE_SIZE * 2);
        CHECK(grow_ret == -1 && (errno == EPERM || errno == EINVAL),
              "F_SEAL_GROW 后 ftruncate 增大失败");

        close(fd);
    }

    /* ================================================================
     * PART 7: F_SEAL_WRITE 生效验证
     * ================================================================ */

    fd = memfd_create("test_write_seal", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(write seal test) 成功");
    if (fd >= 0) {
        ftruncate(fd, PAGE_SIZE);
        /* F_SEAL_WRITE 前需先取消 writable mmap（如有） */
        CHECK_RET(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), 0,
                  "F_ADD_SEALS(F_SEAL_WRITE) 成功");

        /* write 应失败 */
        errno = 0;
        ssize_t w_seal = write(fd, "x", 1);
        CHECK(w_seal == -1 && errno == EPERM,
              "F_SEAL_WRITE 后 write 返回 EPERM");

        close(fd);
    }

    /* ================================================================
     * PART 8: /proc/self/fd 符号链接验证
     * ================================================================ */

    fd = memfd_create("my_test_name", 0);
    CHECK(fd >= 0, "memfd_create(proc link test) 成功");
    if (fd >= 0) {
        char link_path[64];
        char target[256];
        snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
        ssize_t link_len = readlink(link_path, target, sizeof(target) - 1);
        if (link_len > 0) {
            target[link_len] = '\0';
            CHECK(strstr(target, "memfd:my_test_name") != NULL,
                  "/proc/self/fd 链接包含 'memfd:my_test_name'");
        } else {
            /* 无 /proc 时跳过 */
            CHECK(1, "无 /proc 文件系统，跳过符号链接检查");
        }
        close(fd);
    }

    /* ================================================================
     * PART 9: fork 继承验证
     * ================================================================ */

    fd = memfd_create("test_fork", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(fork test) 成功");
    if (fd >= 0) {
        const char *fork_msg = "fork_inherit_data";
        int fork_msg_len = strlen(fork_msg);
        write(fd, fork_msg, fork_msg_len);

        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程: 通过继承的 fd 读取数据 */
            lseek(fd, 0, SEEK_SET);
            char child_buf[64] = {0};
            /* 只读精确字节数，避免读到 ftruncate 填充的零 */
            ssize_t n = read(fd, child_buf, fork_msg_len);
            close(fd);
            _exit((n == fork_msg_len &&
                   memcmp(child_buf, fork_msg, fork_msg_len) == 0) ? 0 : 1);
        }

        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "fork 后子进程通过继承的 fd 读到正确数据");
        close(fd);
    }

    /* ================================================================
     * PART 10: 负向测试 — 错误路径
     * ================================================================ */

    /* 10.1 EFAULT: name 指向无效地址 */
    errno = 0;
    int bad_fd = memfd_create((char *)1, 0);
    CHECK(bad_fd == -1 && (errno == EFAULT || errno == EINVAL),
          "memfd_create 无效 name 地址 -> EFAULT/EINVAL");

    /* 10.2 EINVAL: flags 含未知位 */
    errno = 0;
    bad_fd = memfd_create("test", 0xFF000000);
    CHECK_ERR(memfd_create("test", 0xFF000000), EINVAL,
              "memfd_create 未知 flags 位 -> EINVAL");
    /* 恢复上面那个调用的 bad_fd 变量 */
    (void)bad_fd;

    /* 10.3 EINVAL: name 过长 (>249 字节) */
    char long_name[300];
    memset(long_name, 'A', 299);
    long_name[299] = '\0';
    errno = 0;
    CHECK_ERR(memfd_create(long_name, 0), EINVAL,
              "memfd_create name 过长 -> EINVAL");

    /* 10.4 EINVAL: name 为空字符串（合法但边界情况） */
    fd = memfd_create("", 0);
    CHECK(fd >= 0, "memfd_create 空名字创建成功");
    if (fd >= 0) {
        close(fd);
    }

    /* 10.5 MFD_ALLOW_SEALING + ftruncate + mmap 写 + F_SEAL_WRITE */
    fd = memfd_create("test_seal_write_mmap", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(seal write mmap test)");
    if (fd >= 0) {
        ftruncate(fd, PAGE_SIZE);
        /* 先 mmap 写入 */
        char *mp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
        if (mp != MAP_FAILED) {
            memcpy(mp, "test", 4);
            munmap(mp, PAGE_SIZE);

            /* 取消 mmap 后才能加 F_SEAL_WRITE */
            CHECK_RET(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), 0,
                      "取消 mmap 后 F_ADD_SEALS(F_SEAL_WRITE) 成功");

            /* 再 mmap PROT_WRITE 应失败 */
            errno = 0;
            char *mp2 = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
            CHECK(mp2 == MAP_FAILED && (errno == EPERM || errno == EACCES),
                  "F_SEAL_WRITE 后 mmap PROT_WRITE 失败");
            if (mp2 != MAP_FAILED) munmap(mp2, PAGE_SIZE);
        }
        close(fd);
    }

    /* ================================================================
     * PART 11: 多个 seal 组合验证
     * ================================================================ */

    fd = memfd_create("test_multi_seal", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(multi seal) 成功");
    if (fd >= 0) {
        ftruncate(fd, PAGE_SIZE);
        /* 同时添加多个 seal */
        unsigned int combined = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
        CHECK_RET(fcntl(fd, F_ADD_SEALS, combined), 0,
                  "F_ADD_SEALS 组合 seal 成功");
        int seals = fcntl(fd, F_GET_SEALS);
        CHECK(seals >= 0, "F_GET_SEALS 成功");
        CHECK((seals & F_SEAL_SHRINK) != 0, "包含 F_SEAL_SHRINK");
        CHECK((seals & F_SEAL_GROW) != 0, "包含 F_SEAL_GROW");
        CHECK((seals & F_SEAL_WRITE) != 0, "包含 F_SEAL_WRITE");

        close(fd);
    }

    /* ================================================================
     * PART 12: F_SEAL_SEAL 阻止后续 seal 操作
     * ================================================================ */

    fd = memfd_create("test_seal_seal", MFD_ALLOW_SEALING);
    CHECK(fd >= 0, "memfd_create(seal seal test) 成功");
    if (fd >= 0) {
        CHECK_RET(fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL), 0,
                  "F_ADD_SEALS(F_SEAL_SEAL) 成功");
        errno = 0;
        CHECK_ERR(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK), EPERM,
                  "F_SEAL_SEAL 后无法再添加 seal");
        close(fd);
    }

    TEST_DONE();
}
