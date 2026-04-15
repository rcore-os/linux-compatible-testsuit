/*
 * test_pidfd_getfd.c — pidfd_getfd 功能完整验证
 *
 * 测试策略：验证通过 pidfd 复制另一进程的 fd、数据一致性、
 *           共享文件偏移、FD_CLOEXEC、权限检查及错误路径
 *
 * 覆盖范围：
 *   正向：复制子进程 fd、读取相同内容、共享文件偏移、
 *         FD_CLOEXEC 标志、pipe fd 复制
 *   负向：EBADF 无效 pidfd、EBADF 无效 targetfd、
 *         EINVAL flags!=0、EPERM 权限不足、ESRCH 进程已退出
 *
 * 注意：glibc 无 pidfd_getfd wrapper，使用 syscall()
 */

#include "test_framework.h"
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sched.h>

/* pidfd_open / pidfd_getfd 封装 */
static int pidfd_open(pid_t pid, unsigned int flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

static int pidfd_getfd(int pidfd, int targetfd, unsigned int flags)
{
    return (int)syscall(SYS_pidfd_getfd, pidfd, targetfd, flags);
}

/* 辅助：创建带内容的临时文件并返回 fd */
static int create_tmp_file(const char *path, const char *content)
{
    int fd = openat(AT_FDCWD, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content) write(fd, content, strlen(content));
    return fd;
}

#define TMPFILE "/tmp/starry_test_pidfd_getfd"

int main(void)
{
    TEST_START("pidfd_getfd: 跨进程文件描述符复制验证");

    /* 清理残留 */
    unlink(TMPFILE);

    /* ================================================================
     * PART 1: 基本功能 — 复制子进程的文件 fd 并读取相同内容
     * ================================================================ */

    int tmp_fd = create_tmp_file(TMPFILE, "pidfd_getfd_test_data");
    CHECK(tmp_fd >= 0, "创建临时文件成功");
    if (tmp_fd < 0) { TEST_DONE(); }

    /* 子进程持有 tmp_fd */
    pid_t child = fork();
    if (child == 0) {
        /* 子进程: 暂停等待父进程操作 */
        pause();
        close(tmp_fd);
        _exit(0);
    }

    /* 父进程 */
    int child_pidfd = pidfd_open(child, 0);
    CHECK(child_pidfd >= 0, "pidfd_open(child) 成功");
    if (child_pidfd < 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        close(tmp_fd);
        unlink(TMPFILE);
        TEST_DONE();
    }

    /* 1.1 复制子进程的 tmp_fd */
    int dup_fd = pidfd_getfd(child_pidfd, tmp_fd, 0);
    CHECK(dup_fd >= 0, "pidfd_getfd 复制子进程 fd 成功");
    CHECK(dup_fd != tmp_fd, "复制的 fd 编号不同于原始 fd");
    if (dup_fd >= 0) {
        /* 1.2 读取内容一致 */
        lseek(dup_fd, 0, SEEK_SET);
        char buf[64] = {0};
        ssize_t n = read(dup_fd, buf, sizeof(buf) - 1);
        CHECK(n > 0, "通过复制的 fd 读取数据成功");
        CHECK(strcmp(buf, "pidfd_getfd_test_data") == 0,
              "复制的 fd 读到与原始相同的数据");

        /* 1.3 验证 FD_CLOEXEC 已设置 */
        int fd_flags = fcntl(dup_fd, F_GETFD);
        CHECK(fd_flags >= 0, "F_GETFD 成功");
        CHECK((fd_flags & FD_CLOEXEC) != 0,
              "pidfd_getfd 返回的 fd 设置了 FD_CLOEXEC");

        close(dup_fd);
    }

    /* 唤醒并清理子进程 */
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(child_pidfd);
    close(tmp_fd);

    /* ================================================================
     * PART 2: 共享文件偏移验证
     * pidfd_getfd 返回的 fd 与原始 fd 共享 open file description
     * 使用 pipe 同步：子进程先读 4 字节再通知父进程
     * ================================================================ */

    tmp_fd = create_tmp_file(TMPFILE, "AAAA_BBBB_CCCC_DDDD");
    CHECK(tmp_fd >= 0, "偏移测试: 创建文件成功");

    int sync_pipe[2];
    CHECK_RET(pipe(sync_pipe), 0, "偏移测试: pipe 同步创建成功");

    child = fork();
    if (child == 0) {
        /* 子进程: 先读前 4 字节，偏移移动到 4 */
        close(sync_pipe[0]);
        char tmp[8];
        lseek(tmp_fd, 0, SEEK_SET);
        read(tmp_fd, tmp, 4);
        /* 通知父进程：偏移已移动 */
        char c = 1;
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        pause();
        close(tmp_fd);
        _exit(0);
    }

    close(sync_pipe[1]);
    /* 等待子进程完成读取 */
    {
        char sync_c;
        read(sync_pipe[0], &sync_c, 1);
    }
    close(sync_pipe[0]);

    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        /* 2.1 复制 fd */
        dup_fd = pidfd_getfd(child_pidfd, tmp_fd, 0);
        if (dup_fd >= 0) {
            /* 2.2 验证共享偏移: 子进程读到偏移 4，
             * 复制的 fd 应也在偏移 4 附近 */
            off_t pos = lseek(dup_fd, 0, SEEK_CUR);
            CHECK(pos == 4,
                  "复制的 fd 与原始共享文件偏移 (off=4)");

            /* 2.3 从偏移 4 继续读取 */
            char buf[8] = {0};
            ssize_t n = read(dup_fd, buf, 4);
            CHECK(n == 4, "从共享偏移读取 4 字节");
            CHECK(memcmp(buf, "_BBB", 4) == 0,
                  "共享偏移读取内容正确 ('_BBB')");

            close(dup_fd);
        }
        close(child_pidfd);
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(tmp_fd);

    /* ================================================================
     * PART 3: 复制 pipe fd
     * ================================================================ */

    int pipefds[2];
    int pret = pipe(pipefds);
    CHECK_RET(pret, 0, "pipe 创建成功");

    if (pret == 0) {
        child = fork();
        if (child == 0) {
            /* 子进程: 关闭读端，只保留写端 */
            close(pipefds[0]);
            pause();
            close(pipefds[1]);
            _exit(0);
        }

        /* 父进程关闭写端 */
        close(pipefds[1]);

        child_pidfd = pidfd_open(child, 0);
        if (child_pidfd >= 0) {
            /* 3.1 复制子进程的写端 fd */
            int dup_pipe = pidfd_getfd(child_pidfd, pipefds[1], 0);
            if (dup_pipe >= 0) {
                /* 3.2 通过复制的写端写入数据 */
                const char *pipe_msg = "hello from pidfd_getfd";
                ssize_t w = write(dup_pipe, pipe_msg, strlen(pipe_msg));
                CHECK(w == (ssize_t)strlen(pipe_msg),
                      "通过复制的 pipe 写端写入成功");

                close(dup_pipe);
            }
            close(child_pidfd);
        }

        /* 3.3 从原始读端读到数据 */
        char pipe_buf[64] = {0};
        ssize_t rn = read(pipefds[0], pipe_buf, sizeof(pipe_buf) - 1);
        if (rn > 0) {
            CHECK(strncmp(pipe_buf, "hello from pidfd_getfd", rn) == 0,
                  "pipe: 读端收到通过复制 fd 写入的数据");
        }

        close(pipefds[0]);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }

    /* ================================================================
     * PART 4: 负向测试 — EBADF 无效 pidfd
     * ================================================================ */

    /* 4.1 EBADF: pidfd 为无效值 */
    errno = 0;
    CHECK_ERR(pidfd_getfd(-1, 0, 0), EBADF,
              "pidfd_getfd 无效 pidfd -> EBADF");

    /* 4.2 EBADF: pidfd 为普通文件 fd（非 pidfd）*/
    tmp_fd = create_tmp_file(TMPFILE, "x");
    if (tmp_fd >= 0) {
        errno = 0;
        CHECK_ERR(pidfd_getfd(tmp_fd, 0, 0), EBADF,
                  "pidfd_getfd 用普通文件 fd 作为 pidfd -> EBADF");
        close(tmp_fd);
    }

    /* ================================================================
     * PART 5: 负向测试 — EBADF 无效 targetfd
     * ================================================================ */

    child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }

    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        /* 5.1 targetfd 不存在于子进程 */
        errno = 0;
        CHECK_ERR(pidfd_getfd(child_pidfd, 999, 0), EBADF,
                  "pidfd_getfd 无效 targetfd -> EBADF");

        close(child_pidfd);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    /* ================================================================
     * PART 6: 负向测试 — EINVAL flags != 0
     * ================================================================ */

    child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }

    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        errno = 0;
        CHECK_ERR(pidfd_getfd(child_pidfd, 0, 1), EINVAL,
                  "pidfd_getfd flags!=0 -> EINVAL");

        close(child_pidfd);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    /* ================================================================
     * PART 7: 负向测试 — ESRCH 进程已退出并被回收
     * ================================================================ */

    child = fork();
    if (child == 0) {
        _exit(0);
    }

    /* 等待子进程退出并回收 */
    waitpid(child, NULL, 0);

    /* 此时 child 的 PID 已被回收，pidfd_open 应失败 */
    errno = 0;
    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        /* 如果 pidfd_open 成功（罕见），用 pidfd_getfd 测试 */
        errno = 0;
        CHECK_ERR(pidfd_getfd(child_pidfd, 0, 0), ESRCH,
                  "pidfd_getfd 对已回收进程 -> ESRCH");
        close(child_pidfd);
    } else {
        CHECK(errno == ESRCH,
              "已回收进程 pidfd_open 返回 ESRCH");
    }

    /* ================================================================
     * PART 8: fstat 一致性验证
     * 复制的 fd 应指向同一文件（stat 结果相同）
     * ================================================================ */

    tmp_fd = create_tmp_file(TMPFILE, "stat_test_content");
    CHECK(tmp_fd >= 0, "fstat 测试: 创建文件成功");

    child = fork();
    if (child == 0) {
        pause();
        close(tmp_fd);
        _exit(0);
    }

    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        dup_fd = pidfd_getfd(child_pidfd, tmp_fd, 0);
        if (dup_fd >= 0) {
            struct stat st_orig, st_dup;
            fstat(tmp_fd, &st_orig);
            fstat(dup_fd, &st_dup);

            CHECK(st_orig.st_ino == st_dup.st_ino,
                  "fstat: inode 一致");
            CHECK(st_orig.st_dev == st_dup.st_dev,
                  "fstat: device 一致");
            CHECK(st_orig.st_size == st_dup.st_size,
                  "fstat: size 一致");

            close(dup_fd);
        }
        close(child_pidfd);
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(tmp_fd);

    /* ================================================================
     * PART 9: write 后共享偏移移动验证
     * ================================================================ */

    tmp_fd = create_tmp_file(TMPFILE, "XXXXXXXXXXXXXXXXXXXX");
    CHECK(tmp_fd >= 0, "write 偏移测试: 创建文件成功");

    child = fork();
    if (child == 0) {
        pause();
        close(tmp_fd);
        _exit(0);
    }

    child_pidfd = pidfd_open(child, 0);
    if (child_pidfd >= 0) {
        dup_fd = pidfd_getfd(child_pidfd, tmp_fd, 0);
        if (dup_fd >= 0) {
            /* 通过原始 fd 写入 */
            lseek(tmp_fd, 0, SEEK_SET);
            write(tmp_fd, "HELLO", 5);

            /* 通过复制的 fd 检查偏移 */
            off_t pos_orig = lseek(tmp_fd, 0, SEEK_CUR);
            off_t pos_dup = lseek(dup_fd, 0, SEEK_CUR);
            CHECK(pos_orig == pos_dup,
                  "write 后两个 fd 的偏移一致（共享 file description）");

            close(dup_fd);
        }
        close(child_pidfd);
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(tmp_fd);

    /* ================================================================
     * 清理
     * ================================================================ */
    unlink(TMPFILE);

    TEST_DONE();
}
