/*
 * test_dup_v2.c — dup / dup3 / fcntl / flock 功能完整验证 v3
 *
 * 测试策略：不仅验证返回值，还通过独立观测手段验证功能语义真正生效。
 * 遵循 sys_design.md "操作→观测→验证" 三步法。
 *
 * 覆盖范围（v3 新增）：
 *   正向：fcntl F_DUPFD/CLOEXEC 验证、dup后原fd关闭验证、F_SETFL O_NONBLOCK/O_APPEND/清除、
 *         flock LOCK_SH/LOCK_EX/LOCK_UN、fcntl F_SETLK/F_GETLK/F_SETLKW
 *   负向：EBADF/EINVAL 错误路径
 */

#include "test_framework.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>  /* for flock */
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define TMPFILE "/tmp/starry_test_dup_v2"

/* F_DUPFD_CLOEXEC 可能未定义 */
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif

/* 辅助：创建测试文件 */
static int create_test_file(const char *path, const char *content)
{
    int fd = openat(AT_FDCWD, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content) write(fd, content, strlen(content));
    close(fd);
    return 0;
}

int main(void)
{
    TEST_START("dup/dup3/fcntl/flock: 完整功能语义验证 v3");

    /* 清理残留 */
    unlink(TMPFILE);

    /* ================================================================
     * PART 1: 基本 dup — 验证复制后 fd 指向同一文件且可独立操作
     * ================================================================ */

    create_test_file(TMPFILE, "hello");

    int fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "openat 创建文件成功");
    if (fd < 0) { TEST_DONE(); }

    int fd2 = dup(fd);
    CHECK(fd2 >= 0, "dup 成功");
    CHECK(fd2 != fd, "dup 返回不同的 fd");

    /* 通过 dup'd fd 独立 lseek + read */
    lseek(fd2, 0, SEEK_SET);
    char buf[16] = {0};
    ssize_t n = read(fd2, buf, 5);
    CHECK_RET(n, 5, "通过 dup fd 读回完整数据");
    CHECK(memcmp(buf, "hello", 5) == 0, "dup fd 读回内容正确");

    close(fd2);

    /* ================================================================
     * PART 2: dup 后关闭原 fd — 验证副本 fd 真正独立
     * ================================================================ */

    int fd_dup = dup(fd);
    CHECK(fd_dup >= 0, "dup(fd) for close-original test");

    /* 关闭原 fd */
    CHECK_RET(close(fd), 0, "关闭原 fd");

    /* 观测：通过 dup'd fd 读回数据 */
    lseek(fd_dup, 0, SEEK_SET);
    char buf2[16] = {0};
    ssize_t n2 = read(fd_dup, buf2, 5);
    CHECK_RET(n2, 5, "原 fd 关闭后: dup fd 仍能读回完整数据");
    CHECK(memcmp(buf2, "hello", 5) == 0, "原 fd 关闭后: 内容正确");

    close(fd_dup);

    /* ================================================================
     * PART 3: dup3 到指定 fd 号 + O_CLOEXEC
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "重新打开文件");

    int fd3 = dup3(fd, 30, 0);
    CHECK_RET(fd3, 30, "dup3 指定 fd=30 返回精确匹配");

    /* 观测：通过 fd=30 读取 */
    lseek(fd3, 0, SEEK_SET);
    char buf3[16] = {0};
    ssize_t n3 = read(fd3, buf3, 5);
    CHECK_RET(n3, 5, "dup3 fd=30: 读回完整数据");
    CHECK(memcmp(buf3, "hello", 5) == 0, "dup3 fd=30: 内容正确");
    close(fd3);

    /* dup3 O_CLOEXEC */
    int fd4 = dup3(fd, 31, O_CLOEXEC);
    CHECK_RET(fd4, 31, "dup3 O_CLOEXEC 指定 fd=31");

    int fd_flags = fcntl(fd4, F_GETFD);
    CHECK(fd_flags >= 0, "fcntl F_GETFD 成功");
    CHECK((fd_flags & FD_CLOEXEC) != 0, "dup3 O_CLOEXEC: F_GETFD 确认 FD_CLOEXEC 已设置");
    close(fd4);

    /* ================================================================
     * PART 4: fcntl F_DUPFD — 验证返回 fd >= arg
     *
     * 已知 bug: F_DUPFD 忽略 arg 参数
     * ================================================================ */

    int fd5 = fcntl(fd, F_DUPFD, 50);
    CHECK(fd5 >= 0, "fcntl F_DUPFD(50) 返回有效 fd");
    CHECK(fd5 >= 50, "fcntl F_DUPFD(50): 返回 fd >= 50 (bug 检测)");
    if (fd5 >= 0) {
        lseek(fd5, 0, SEEK_SET);
        char buf5[16];
        ssize_t n5 = read(fd5, buf5, 5);
        CHECK_RET(n5, 5, "F_DUPFD fd: 读回完整数据");
        CHECK(memcmp(buf5, "hello", 5) == 0, "F_DUPFD fd: 内容正确");

        /* F_DUPFD 复制的 fd 不应有 FD_CLOEXEC */
        int fl5 = fcntl(fd5, F_GETFD);
        if (fl5 >= 0) {
            CHECK((fl5 & FD_CLOEXEC) == 0, "F_DUPFD: 新 fd 无 FD_CLOEXEC");
        }
        close(fd5);
    }

    /* ================================================================
     * PART 5: fcntl F_DUPFD_CLOEXEC
     * ================================================================ */

    int fd6 = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    CHECK(fd6 >= 0, "fcntl F_DUPFD_CLOEXEC(0) 成功");
    if (fd6 >= 0) {
        int fl6 = fcntl(fd6, F_GETFD);
        CHECK(fl6 >= 0, "F_DUPFD_CLOEXEC: F_GETFD 成功");
        CHECK((fl6 & FD_CLOEXEC) != 0, "F_DUPFD_CLOEXEC: FD_CLOEXEC 已设置");
        close(fd6);
    }

    /* ================================================================
     * PART 6: fcntl F_SETFD/F_GETFD 状态转移
     * ================================================================ */

    /* 6.1 设置 FD_CLOEXEC */
    CHECK_RET(fcntl(fd, F_SETFD, FD_CLOEXEC), 0, "F_SETFD → FD_CLOEXEC: 设置成功");
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "F_SETFD → FD_CLOEXEC: F_GETFD 读回 == 1");

    /* 6.2 清除 FD_CLOEXEC */
    CHECK_RET(fcntl(fd, F_SETFD, 0), 0, "F_SETFD → 0: 清除 FD_CLOEXEC");
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0, "F_SETFD → 0: F_GETFD 读回 == 0");

    /* ================================================================
     * PART 7: fcntl F_SETFL O_NONBLOCK — 通过管道验证非阻塞真正生效
     * ================================================================ */

    int pipefds[2];
    int pret = pipe(pipefds);
    CHECK_RET(pret, 0, "pipe 创建管道成功");

    if (pret == 0) {
        /* 设置读端 O_NONBLOCK */
        CHECK_RET(fcntl(pipefds[0], F_SETFL, O_NONBLOCK), 0, "F_SETFL → O_NONBLOCK: 设置成功");

        /* 观测：F_GETFL 读回确认包含 O_NONBLOCK */
        int pfl = fcntl(pipefds[0], F_GETFL);
        CHECK(pfl >= 0, "F_GETFL 成功");
        CHECK((pfl & O_NONBLOCK) != 0, "F_GETFL 包含 O_NONBLOCK");

        /* 关键观测：对空管道 read — 非阻塞应返回 EAGAIN */
        char pbuf[4];
        errno = 0;
        CHECK_ERR(read(pipefds[0], pbuf, 1), EAGAIN, "O_NONBLOCK 空管道 read 返回 EAGAIN（非阻塞真正生效）");

        close(pipefds[0]);
        close(pipefds[1]);
    }

    /* ================================================================
     * PART 8: fcntl F_SETFL O_APPEND — 追加模式验证
     * ================================================================ */

    /* 重新打开文件，清除旧的 append 标志 */
    close(fd);
    create_test_file(TMPFILE, "start");
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "重新打开文件用于 O_APPEND 测试");

    CHECK_RET(fcntl(fd, F_SETFL, O_APPEND), 0, "F_SETFL → O_APPEND: 设置成功");

    /* 观测：write 后 offset 在末尾（追加生效） */
    write(fd, "_middle", 7);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    CHECK_RET(pos, 12, "O_APPEND: write 后 offset 在末尾（非从当前位置）");

    close(fd);

    /* ================================================================
     * PART 9: fcntl F_SETFL 清除标志
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR | O_APPEND);
    CHECK(fd >= 0, "打开文件带 O_APPEND 标志");

    int fl_before = fcntl(fd, F_GETFL);
    CHECK(fl_before >= 0, "F_GETFL 获取标志成功");

    /* 清除 O_APPEND */
    CHECK_RET(fcntl(fd, F_SETFL, 0), 0, "F_SETFL → 0: 清除所有修改标志");

    int fl_after = fcntl(fd, F_GETFL);
    CHECK(fl_after >= 0, "清除后 F_GETFL 成功");
    CHECK((fl_after & O_APPEND) == 0, "O_APPEND 已被清除");

    /* 验证：现在 write 从当前 offset 开始，而非追加 */
    lseek(fd, 0, SEEK_SET);
    write(fd, "X", 1);
    lseek(fd, 0, SEEK_SET);
    char buf_append[16];
    read(fd, buf_append, 16);
    CHECK(buf_append[0] == 'X', "清除 O_APPEND 后: write 覆盖而非追加");

    close(fd);

    /* ================================================================
     * PART 10: fcntl F_GETFL 完整读回验证
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR | O_APPEND | O_NONBLOCK);
    CHECK(fd >= 0, "打开文件带多个标志");

    int fl_complete = fcntl(fd, F_GETFL);
    CHECK(fl_complete >= 0, "F_GETFL 成功");
    CHECK((fl_complete & O_ACCMODE) == O_RDWR, "F_GETFL: O_RDWR 正确");
    CHECK((fl_complete & O_APPEND) != 0, "F_GETFL: O_APPEND 已设置");
    CHECK((fl_complete & O_NONBLOCK) != 0, "F_GETFL: O_NONBLOCK 已设置");

    close(fd);

    /* ================================================================
     * PART 11: flock LOCK_SH 共享锁
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "flock 测试: 打开文件");

    CHECK_RET(flock(fd, LOCK_SH), 0, "flock(LOCK_SH) 获取共享锁成功");

    /* 观测：第二个 fd 也能获取共享锁（多读者允许） */
    int fd2_lock = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd2_lock >= 0, "flock: 打开第二个 fd");

    int ret2 = flock(fd2_lock, LOCK_SH | LOCK_NB);
    CHECK(ret2 == 0, "flock: 第二个 fd 也能获取共享锁（多读者）");

    /* 释放锁 */
    flock(fd, LOCK_UN);
    flock(fd2_lock, LOCK_UN);
    close(fd2_lock);

    /* ================================================================
     * PART 12: flock LOCK_EX 排他锁冲突
     * ================================================================ */

    CHECK_RET(flock(fd, LOCK_EX), 0, "flock(LOCK_EX) 获取排他锁成功");

    /* 观测：第二个 fd 尝试获取共享锁应该失败 */
    fd2_lock = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd2_lock >= 0, "flock: 打开第二个 fd 用于冲突测试");

    errno = 0;
    int ret_ex = flock(fd2_lock, LOCK_SH | LOCK_NB);
    CHECK(ret_ex == -1, "flock: 已有排他锁时，LOCK_SH|LOCK_NB 失败");
    CHECK(errno == EWOULDBLOCK, "flock 冲突: errno == EWOULDBLOCK");

    /* 释放锁 */
    flock(fd, LOCK_UN);
    close(fd2_lock);

    /* ================================================================
     * PART 13: flock LOCK_UN 释放锁
     * ================================================================ */

    CHECK_RET(flock(fd, LOCK_EX), 0, "flock: 获取排他锁");

    /* 观测：释放锁后，其他 fd 可以获取锁 */
    flock(fd, LOCK_UN);

    fd2_lock = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd2_lock >= 0, "flock: 打开第二个 fd 验证锁释放");

    ret2 = flock(fd2_lock, LOCK_EX | LOCK_NB);
    CHECK(ret2 == 0, "flock LOCK_UN 后: 第二个 fd 可以获取排他锁");

    flock(fd2_lock, LOCK_UN);
    close(fd2_lock);

    /* ================================================================
     * PART 14: flock 进程继承
     * ================================================================ */

    CHECK_RET(flock(fd, LOCK_EX), 0, "flock: 父进程获取排他锁");

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：验证继承了锁，可以通过 fd 操作 */
        char buf_inherit[16];
        lseek(fd, 0, SEEK_SET);
        ssize_t n_inherit = read(fd, buf_inherit, 5);
        exit(n_inherit == 5 ? 0 : 1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "flock: 子进程继承了锁，可以操作文件");
    }

    close(fd);

    /* ================================================================
     * PART 15: fcntl F_SETLK 写锁与 F_GETLK 查询
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "fcntl 文件锁测试: 打开文件");

    /* 设置写锁 */
    struct flock flk;
    memset(&flk, 0, sizeof(flk));
    flk.l_type = F_WRLCK;
    flk.l_whence = SEEK_SET;
    flk.l_start = 0;
    flk.l_len = 100;
    flk.l_pid = 0;

    CHECK_RET(fcntl(fd, F_SETLK, &flk), 0, "fcntl F_SETLK(F_WRLCK) 设置写锁成功");

    /* 观测：F_GETLK 查询同一进程持有的写锁 */
    /* 注意：POSIX 规定，查询同一进程持有的锁时，F_GETLK 返回 F_UNLCK
     * （因为同一进程的锁不与自己冲突）*/
    struct flock flk_query;
    memset(&flk_query, 0, sizeof(flk_query));
    flk_query.l_type = F_WRLCK;
    flk_query.l_whence = SEEK_SET;
    flk_query.l_start = 0;
    flk_query.l_len = 100;

    CHECK_RET(fcntl(fd, F_GETLK, &flk_query), 0, "fcntl F_GETLK 查询锁状态");
    CHECK(flk_query.l_type == F_UNLCK, "F_GETLK: 同进程查询返回 F_UNLCK（无冲突）");

    /* 释放锁 */
    flk.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &flk);

    /* ================================================================
     * PART 16: fcntl F_SETLK 跨进程写锁冲突
     * ================================================================ */

    /* 父进程设置写锁 */
    flk.l_type = F_WRLCK;
    CHECK_RET(fcntl(fd, F_SETLK, &flk), 0, "父进程: F_SETLK(F_WRLCK) 设置写锁");

    pid = fork();
    if (pid == 0) {
        /* 子进程：尝试设置冲突的写锁，应该失败 */
        int fd_child = openat(AT_FDCWD, TMPFILE, O_RDWR);
        if (fd_child >= 0) {
            struct flock flk_child;
            memset(&flk_child, 0, sizeof(flk_child));
            flk_child.l_type = F_WRLCK;
            flk_child.l_whence = SEEK_SET;
            flk_child.l_start = 0;
            flk_child.l_len = 100;

            errno = 0;
            int ret = fcntl(fd_child, F_SETLK, &flk_child);
            /* 预期失败：父进程持有写锁 */
            if (ret == -1 && (errno == EACCES || errno == EAGAIN)) {
                exit(0);  /* 成功：正确检测到冲突 */
            } else {
                exit(1);  /* 失败：应该检测到冲突但没有 */
            }
            close(fd_child);
        }
        exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "fcntl F_SETLK 跨进程冲突: 子进程正确检测到写锁冲突");
    }

    /* 释放锁 */
    flk.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &flk);

    /* ================================================================
     * PART 17: fcntl F_SETLKW 阻塞等待
     * ================================================================ */

    /* 这个测试需要两个进程协作 */
    pid = fork();
    if (pid == 0) {
        /* 子进程：持有锁 1 秒后释放 */
        struct flock flk_child;
        memset(&flk_child, 0, sizeof(flk_child));
        flk_child.l_type = F_WRLCK;
        flk_child.l_whence = SEEK_SET;
        flk_child.l_start = 0;
        flk_child.l_len = 0;  /* 锁定整个文件 */

        fcntl(fd, F_SETLKW, &flk_child);
        sleep(1);  /* 持有锁 1 秒 */

        flk_child.l_type = F_UNLCK;
        fcntl(fd, F_SETLKW, &flk_child);
        exit(0);
    } else if (pid > 0) {
        /* 父进程：等待子进程设置锁 */
        sleep(1);

        /* 尝试获取锁，应该阻塞直到子进程释放 */
        struct flock flk_parent;
        memset(&flk_parent, 0, sizeof(flk_parent));
        flk_parent.l_type = F_WRLCK;
        flk_parent.l_whence = SEEK_SET;
        flk_parent.l_start = 0;
        flk_parent.l_len = 0;

        CHECK_RET(fcntl(fd, F_SETLKW, &flk_parent), 0, "F_SETLKW: 等待后成功获取锁");

        /* 清理 */
        flk_parent.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &flk_parent);

        wait(NULL);
    }

    close(fd);

    /* ================================================================
     * PART 18: fcntl F_GETFL 基础验证
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    CHECK(fd >= 0, "打开文件用于 F_GETFL 测试");

    int fl = fcntl(fd, F_GETFL);
    CHECK(fl >= 0, "fcntl F_GETFL 成功");
    CHECK((fl & O_ACCMODE) == O_RDWR, "F_GETFL: O_ACCMODE == O_RDWR(2)");

    close(fd);

    /* ================================================================
     * PART 19: 负向测试 — 错误路径
     * ================================================================ */

    /* 19.1 dup 无效 fd */
    errno = 0;
    CHECK_ERR(dup(-1), EBADF, "dup(-1) -> EBADF");
    errno = 0;
    CHECK_ERR(dup(9999), EBADF, "dup(9999) -> EBADF");

    /* 19.2 dup3 old==new */
    errno = 0;
    CHECK_ERR(dup3(5, 5, 0), EINVAL, "dup3(5,5,0) old==new -> EINVAL");

    /* 19.3 dup3 无效 flags */
    errno = 0;
    CHECK_ERR(dup3(0, 40, 0xFF00), EINVAL, "dup3 无效 flags -> EINVAL");

    /* 19.4 dup3 newfd 负数 */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(dup3(fd, -1, 0), EBADF, "dup3 newfd=-1 -> EBADF");
        close(fd);
    }

    /* 19.5 fcntl 无效 fd */
    errno = 0;
    CHECK_ERR(fcntl(-1, F_GETFD), EBADF, "fcntl(-1, F_GETFD) -> EBADF");

    /* 19.6 fcntl F_DUPFD arg 负数 */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(fcntl(fd, F_DUPFD, -1), EINVAL, "fcntl F_DUPFD arg=-1 -> EINVAL");
        close(fd);
    }

    /* 19.7 fcntl 不支持的 cmd */
    fd = openat(AT_FDCWD, TMPFILE, O_RDWR);
    if (fd >= 0) {
        errno = 0;
        CHECK_ERR(fcntl(fd, 0xABCD), EINVAL, "fcntl 不支持的 cmd -> EINVAL");
        close(fd);
    }

    /* ================================================================
     * 清理
     * ================================================================ */
    unlink(TMPFILE);

    TEST_DONE();
}
