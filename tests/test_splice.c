#include "test_framework.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TEST_FILE_1 "/tmp/test_splice_1.tmp"
#define TEST_FILE_2 "/tmp/test_splice_2.tmp"

/* 子进程退出状态码 */
#define STATUS_CHILD_OK   10

int main(void) {
    TEST_START("splice: splice data to/from a pipe");

    /* ==================== 正向测试 ==================== */

    /*
     * 测试 1: splice 基本功能 — 文件 → 管道
     * 手册描述: splice() moves data between two file descriptors without
     *           copying between kernel address space and user address space.
     *           Upon successful completion, returns the number of bytes spliced.
     */
    {
        /* 创建文件并写入数据 */
        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0, "创建测试文件成功");
        const char *data = "hello splice";
        write(fd, data, strlen(data));
        lseek(fd, 0, SEEK_SET);

        int pipefd[2];
        pipe(pipefd);

        /* splice: fd_in=文件 → fd_out=管道写端 */
        errno = 0;
        ssize_t n = splice(fd, NULL, pipefd[1], NULL, strlen(data), 0);
        CHECK(n == (ssize_t)strlen(data),
              "splice 文件→管道 返回正确字节数");

        /* 从管道读端读出验证 */
        char buf[64] = {0};
        read(pipefd[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, data) == 0,
              "splice 后从管道读出内容与写入一致");

        close(fd);
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 2: splice 管道 → 文件
     * 手册描述: transfers up to len bytes of data from fd_in to fd_out.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        /* 向管道写入数据 */
        const char *data = "pipe_to_file";
        write(pipefd[1], data, strlen(data));
        close(pipefd[1]);  /* 关闭写端，让 splice 读到 EOF */

        int fd = open(TEST_FILE_2, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0, "创建输出文件成功");

        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, fd, NULL, strlen(data), 0);
        CHECK(n == (ssize_t)strlen(data),
              "splice 管道→文件 返回正确字节数");

        /* 从文件读回验证 */
        char buf[64] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, data) == 0,
              "splice 后文件内容与管道数据一致");

        close(fd);
        close(pipefd[0]);
        unlink(TEST_FILE_2);
    }

    /*
     * 测试 3: splice 管道 → 管道
     * 手册描述: Since Linux 2.6.31, both arguments may refer to pipes.
     */
    {
        int pipe1[2], pipe2[2];
        pipe(pipe1);
        pipe(pipe2);

        const char *data = "pipe2pipe";
        write(pipe1[1], data, strlen(data));
        close(pipe1[1]);

        errno = 0;
        ssize_t n = splice(pipe1[0], NULL, pipe2[1], NULL, strlen(data), 0);
        CHECK(n == (ssize_t)strlen(data),
              "splice 管道→管道 返回正确字节数");

        char buf[64] = {0};
        read(pipe2[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, data) == 0,
              "splice 管道→管道 数据正确传递");

        close(pipe1[0]);
        close(pipe2[0]);
        close(pipe2[1]);
    }

    /*
     * 测试 4: splice 使用 off_in 指定文件偏移量（不改变文件 offset）
     * 手册描述: off_in must point to a buffer which specifies the starting
     *           offset; in this case, the file offset of fd_in is not changed.
     */
    {
        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        const char *data = "ABCDEFGHIJ";  /* 10 bytes */
        write(fd, data, strlen(data));

        int pipefd[2];
        pipe(pipefd);

        /* 从偏移量 5 开始读取 5 字节 (FGHIJ) */
        off_t off = 5;
        errno = 0;
        ssize_t n = splice(fd, &off, pipefd[1], NULL, 5, 0);
        CHECK(n == 5, "splice 带偏移量读取 5 字节成功");

        /* 验证文件偏移量未改变（应仍在文件末尾） */
        off_t cur_off = lseek(fd, 0, SEEK_CUR);
        CHECK(cur_off == (off_t)strlen(data),
              "splice 使用 off_in 后文件偏移量未改变");

        /* 验证读出的数据 */
        char buf[16] = {0};
        read(pipefd[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "FGHIJ") == 0,
              "splice off_in=5 读取的数据正确 (FGHIJ)");

        close(fd);
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 5: splice 使用 off_out 指定输出偏移量
     * 手册描述: Analogous statements apply for fd_out and off_out.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        const char *data = "SPLICE_OFF";
        write(pipefd[1], data, strlen(data));
        close(pipefd[1]);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        /* 先写一些数据 */
        write(fd, "XXXXX", 5);

        /* splice 到偏移量 5 处 */
        off_t off = 5;
        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, fd, &off, strlen(data), 0);
        CHECK(n == (ssize_t)strlen(data),
              "splice 带 off_out 写入成功");

        /* 验证文件内容 */
        char buf[32] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf) - 1);
        CHECK(memcmp(buf, "XXXXX", 5) == 0,
              "splice off_out 后原数据未变");
        CHECK(strcmp(buf + 5, data) == 0,
              "splice off_out 后新数据在偏移 5 处正确写入");

        close(fd);
        close(pipefd[0]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 6: splice 返回 0 表示 EOF（管道读端无数据且写端关闭）
     * 手册描述: A return value of 0 means end of input. If fd_in refers to a
     *           pipe, this means there was no data to transfer.
     */
    {
        int pipefd[2];
        pipe(pipefd);
        close(pipefd[1]);  /* 关闭写端 */

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);

        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, fd, NULL, 1024, 0);
        CHECK_RET(n, 0, "splice 空管道(写端关闭)返回 0 (EOF)");

        close(fd);
        close(pipefd[0]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 7: splice 部分读取 — len 大于可用数据
     * 手册描述: transfers up to len bytes.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        write(pipefd[1], "ABCD", 4);
        close(pipefd[1]);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);

        errno = 0;
        /* 请求 1024 字节，但管道中只有 4 字节 */
        ssize_t n = splice(pipefd[0], NULL, fd, NULL, 1024, 0);
        CHECK(n == 4, "splice len=1024 但管道仅 4 字节，返回 4");

        char buf[16] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "ABCD") == 0,
              "splice 部分读取数据正确");

        close(fd);
        close(pipefd[0]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 8: splice SPLICE_F_MOVE 标志（提示，内核可忽略）
     * 手册描述: Attempt to move pages instead of copying. This is only a hint.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        write(fd, "move_hint", 9);
        lseek(fd, 0, SEEK_SET);

        errno = 0;
        ssize_t n = splice(fd, NULL, pipefd[1], NULL, 9, SPLICE_F_MOVE);
        CHECK(n == 9, "splice SPLICE_F_MOVE 成功");

        char buf[16] = {0};
        read(pipefd[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "move_hint") == 0,
              "splice SPLICE_F_MOVE 数据正确");

        close(fd);
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 9: splice SPLICE_F_MORE 标志
     * 手册描述: More data will be coming in a subsequent splice.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        write(fd, "more_flag", 9);
        lseek(fd, 0, SEEK_SET);

        errno = 0;
        ssize_t n = splice(fd, NULL, pipefd[1], NULL, 9, SPLICE_F_MORE);
        CHECK(n == 9, "splice SPLICE_F_MORE 成功");

        char buf[16] = {0};
        read(pipefd[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "more_flag") == 0,
              "splice SPLICE_F_MORE 数据正确");

        close(fd);
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 10: splice 多次调用实现完整文件传输
     * 验证：分批 splice + 管道中转，传输完整文件
     */
    {
        /* 创建一个较大文件 */
        int src = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        char large_data[4096];
        for (int i = 0; i < (int)sizeof(large_data); i++)
            large_data[i] = 'A' + (i % 26);
        write(src, large_data, sizeof(large_data));
        lseek(src, 0, SEEK_SET);

        int dst = open(TEST_FILE_2, O_RDWR | O_CREAT | O_TRUNC, 0644);

        int pipefd[2];
        pipe(pipefd);

        ssize_t total = 0;
        while (total < (ssize_t)sizeof(large_data)) {
            /* 文件 → 管道 */
            ssize_t n = splice(src, NULL, pipefd[1], NULL,
                               sizeof(large_data) - total, 0);
            if (n <= 0) break;
            /* 管道 → 文件 */
            ssize_t m = splice(pipefd[0], NULL, dst, NULL, n, 0);
            if (m <= 0) break;
            total += m;
        }

        CHECK(total == (ssize_t)sizeof(large_data),
              "splice 多次调用传输完整 4096 字节文件");

        /* 验证内容 */
        char readback[sizeof(large_data)];
        lseek(dst, 0, SEEK_SET);
        read(dst, readback, sizeof(readback));
        CHECK(memcmp(readback, large_data, sizeof(large_data)) == 0,
              "splice 传输后文件内容一致");

        close(src);
        close(dst);
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(TEST_FILE_1);
        unlink(TEST_FILE_2);
    }

    /*
     * 测试 11: splice 跨进程 — 子进程写管道，父进程 splice 到文件
     */
    {
        int pipefd[2];
        pipe(pipefd);

        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            const char *msg = "from_child";
            write(pipefd[1], msg, strlen(msg));
            close(pipefd[1]);
            _exit(STATUS_CHILD_OK);
        }

        close(pipefd[1]);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, fd, NULL, 64, 0);
        CHECK(n > 0, "splice 跨进程 从子进程管道读取数据成功");

        char buf[64] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "from_child") == 0,
              "splice 跨进程数据正确");

        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == STATUS_CHILD_OK,
              "子进程正常退出");

        close(fd);
        close(pipefd[0]);
        unlink(TEST_FILE_1);
    }

    /* ==================== 反向测试 ==================== */

    /*
     * 测试 12: EBADF — 无效的文件描述符
     * 手册描述: One or both file descriptors are not valid.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        CHECK_ERR(splice(-1, NULL, pipefd[1], NULL, 100, 0),
                  EBADF, "splice fd_in=-1 返回 EBADF");

        CHECK_ERR(splice(pipefd[0], NULL, -1, NULL, 100, 0),
                  EBADF, "splice fd_out=-1 返回 EBADF");

        CHECK_ERR(splice(-1, NULL, -1, NULL, 100, 0),
                  EBADF, "splice 两个 fd 均=-1 返回 EBADF");

        close(pipefd[0]);
        close(pipefd[1]);
    }

    /*
     * 测试 13: EBADF — fd 权限不对（读端不可写、写端不可读）
     * 手册描述: do not have proper read-write mode.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        /* pipefd[0] 是读端，不能作为 fd_out（写）用于 splice */
        /* pipefd[1] 是写端，不能作为 fd_in（读）用于 splice */
        int tmpfd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);

        /* pipefd[1] 作为 fd_in：写端不可读 */
        CHECK_ERR(splice(pipefd[1], NULL, tmpfd, NULL, 100, 0),
                  EBADF, "splice fd_in=管道写端(不可读) 返回 EBADF");

        /* pipefd[0] 作为 fd_out：读端不可写 */
        CHECK_ERR(splice(tmpfd, NULL, pipefd[0], NULL, 100, 0),
                  EBADF, "splice fd_out=管道读端(不可写) 返回 EBADF");

        close(pipefd[0]);
        close(pipefd[1]);
        close(tmpfd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 14: EINVAL — 两个 fd 都不是管道
     * 手册描述: Neither of the file descriptors refers to a pipe.
     */
    {
        int fd1 = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        int fd2 = open(TEST_FILE_2, O_RDWR | O_CREAT | O_TRUNC, 0644);

        CHECK_ERR(splice(fd1, NULL, fd2, NULL, 100, 0),
                  EINVAL, "splice 两个文件 fd 均非管道返回 EINVAL");

        close(fd1);
        close(fd2);
        unlink(TEST_FILE_1);
        unlink(TEST_FILE_2);
    }

    /*
     * 测试 15: EINVAL — fd_in 和 fd_out 指向同一个管道
     * 手册描述: fd_in and fd_out refer to the same pipe.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        CHECK_ERR(splice(pipefd[0], NULL, pipefd[1], NULL, 100, 0),
                  EINVAL, "splice 同一管道的读写端返回 EINVAL");

        close(pipefd[0]);
        close(pipefd[1]);
    }

    /*
     * 测试 16: ESPIPE — off_in 非 NULL 但 fd_in 是管道
     * 手册描述: Either off_in or off_out was not NULL, but the corresponding
     *           file descriptor refers to a pipe.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        off_t off = 0;

        CHECK_ERR(splice(pipefd[0], &off, fd, NULL, 100, 0),
                  ESPIPE, "splice off_in 非 NULL 但 fd_in 为管道返回 ESPIPE");

        close(pipefd[0]);
        close(pipefd[1]);
        close(fd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 17: ESPIPE — off_out 非 NULL 但 fd_out 是管道
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);
        off_t off = 0;

        CHECK_ERR(splice(fd, NULL, pipefd[1], &off, 100, 0),
                  ESPIPE, "splice off_out 非 NULL 但 fd_out 为管道返回 ESPIPE");

        close(pipefd[0]);
        close(pipefd[1]);
        close(fd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 18: EINVAL — 目标文件以 O_APPEND 模式打开
     * 手册描述: The target file is opened in append mode.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0644);

        CHECK_ERR(splice(pipefd[0], NULL, fd, NULL, 100, 0),
                  EINVAL, "splice fd_out 以 O_APPEND 打开返回 EINVAL");

        close(pipefd[0]);
        close(pipefd[1]);
        close(fd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 19: EINVAL — len 为 0
     * 验证：len=0 时 splice 返回 EINVAL 或 0
     */
    {
        int pipefd[2];
        pipe(pipefd);

        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, pipefd[1], NULL, 0, 0);
        /* len=0 在某些内核版本返回 0，某些返回 EINVAL，都算合理 */
        CHECK(n == 0 || (n == -1 && errno == EINVAL),
              "splice len=0 返回 0 或 EINVAL (均为合理行为)");

        close(pipefd[0]);
        close(pipefd[1]);
    }

    /*
     * 测试 20: EAGAIN — SPLICE_F_NONBLOCK 且管道无数据
     * 手册描述: SPLICE_F_NONBLOCK was specified and the operation would block.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);

        /* 管道读端无数据，使用 SPLICE_F_NONBLOCK */
        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, fd, NULL, 100, SPLICE_F_NONBLOCK);
        if (n == -1 && errno == EAGAIN) {
            printf("  PASS | %s:%d | splice SPLICE_F_NONBLOCK 管道空返回 EAGAIN (errno=%d as expected)\n",
                   __FILE__, __LINE__, errno);
            __pass++;
        } else if (n == 0) {
            /* 某些实现可能返回 0（无数据可读） */
            printf("  PASS | %s:%d | splice SPLICE_F_NONBLOCK 管道空返回 0 (无数据)\n",
                   __FILE__, __LINE__);
            __pass++;
        } else {
            printf("  FAIL | %s:%d | splice SPLICE_F_NONBLOCK 管道空 | expected EAGAIN or 0, got ret=%zd errno=%d (%s)\n",
                   __FILE__, __LINE__, n, errno, strerror(errno));
            __fail++;
        }

        close(pipefd[0]);
        close(pipefd[1]);
        close(fd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 21: EBADF — 已关闭的 fd
     */
    {
        int pipefd[2];
        pipe(pipefd);
        int fd = open(TEST_FILE_1, O_RDWR | O_CREAT | O_TRUNC, 0644);

        close(pipefd[0]);
        CHECK_ERR(splice(pipefd[0], NULL, fd, NULL, 100, 0),
                  EBADF, "splice 已关闭的 fd_in 返回 EBADF");

        close(pipefd[1]);
        close(fd);
        unlink(TEST_FILE_1);
    }

    /*
     * 测试 22: splice 到 socket（管道 → socketpair）
     * 手册描述: one of the file descriptors must refer to a pipe.
     * socketpair 不是管道，但 splice 允许管道→socket 传输。
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

        const char *data = "to_socket";
        write(pipefd[1], data, strlen(data));
        close(pipefd[1]);

        errno = 0;
        ssize_t n = splice(pipefd[0], NULL, sv[1], NULL, strlen(data), 0);
        CHECK(n == (ssize_t)strlen(data),
              "splice 管道→socket 成功");

        char buf[64] = {0};
        read(sv[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, data) == 0,
              "splice 管道→socket 数据正确");

        close(pipefd[0]);
        close(sv[0]);
        close(sv[1]);
    }

    /* ==================== 清理 ==================== */
    unlink(TEST_FILE_1);
    unlink(TEST_FILE_2);

    TEST_DONE();
}
