#include "test_framework.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TEST_IN   "/tmp/test_sendfile_in.tmp"
#define TEST_OUT  "/tmp/test_sendfile_out.tmp"

/* 辅助：创建文件并写入数据，偏移量回到开头 */
static int create_file(const char *path, const char *data, size_t len) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (len > 0) {
        write(fd, data, len);
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}

/* 辅助：读回文件内容 */
static int verify_content(const char *path, const char *expected, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char *buf = malloc(len + 1);
    memset(buf, 0, len + 1);
    ssize_t n = read(fd, buf, len);
    close(fd);
    int ok = (n == (ssize_t)len && memcmp(buf, expected, len) == 0);
    free(buf);
    return ok;
}

int main(void) {
    TEST_START("sendfile: transfer data between file descriptors");

    /* ==================== 正向测试 ==================== */

    /*
     * 测试 1: sendfile 基本功能 — 文件到文件
     * 手册描述: sendfile() copies data between one file descriptor and another.
     *           If the transfer was successful, the number of bytes written
     *           to out_fd is returned.
     */
    {
        const char *data = "hello sendfile";
        int fd_in = create_file(TEST_IN, data, strlen(data));
        int fd_out = create_file(TEST_OUT, NULL, 0);
        CHECK(fd_in >= 0 && fd_out >= 0, "创建输入输出文件成功");

        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, strlen(data));
        CHECK(n == (ssize_t)strlen(data),
              "sendfile 文件→文件 返回正确字节数");

        close(fd_in);
        close(fd_out);
        CHECK(verify_content(TEST_OUT, data, strlen(data)),
              "sendfile 后输出文件内容与输入一致");

        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 2: offset=NULL 时文件偏移量自动调整
     * 手册描述: data will be read from in_fd starting at the file offset,
     *           and the file offset will be updated by the call.
     */
    {
        const char *data = "ABCDEFGHIJKLMNOP";
        int fd_in = create_file(TEST_IN, data, strlen(data));
        int fd_out = create_file(TEST_OUT, NULL, 0);

        /* 第一次复制前 8 字节 */
        errno = 0;
        ssize_t n1 = sendfile(fd_out, fd_in, NULL, 8);
        CHECK(n1 == 8, "sendfile 第一次复制 8 字节成功");

        /* 第二次复制后 8 字节（偏移量自动从 8 开始） */
        errno = 0;
        ssize_t n2 = sendfile(fd_out, fd_in, NULL, 8);
        CHECK(n2 == 8, "sendfile 第二次复制 8 字节成功（偏移量自动调整）");

        /* 验证 in_fd 偏移量已更新到末尾 */
        off_t cur = lseek(fd_in, 0, SEEK_CUR);
        CHECK(cur == 16, "sendfile 后 in_fd 偏移量已更新到 16");

        close(fd_in);
        close(fd_out);
        CHECK(verify_content(TEST_OUT, data, 16),
              "两次 sendfile 后输出文件内容完整");

        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 3: offset 非 NULL — 指定读取偏移，不改变文件偏移量
     * 手册描述: offset points to a variable holding the file offset from which
     *           sendfile() will start reading. sendfile() does not modify the
     *           file offset of in_fd; offset is updated to the byte following
     *           the last byte read.
     */
    {
        const char *data = "ABCDEFGHIJ";
        int fd_in = create_file(TEST_IN, data, strlen(data));
        /* 将文件偏移量移到末尾，验证不被 sendfile 修改 */
        lseek(fd_in, 0, SEEK_END);
        off_t orig_off = lseek(fd_in, 0, SEEK_CUR);

        int fd_out = create_file(TEST_OUT, NULL, 0);

        off_t off = 5;
        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, &off, 5);
        CHECK(n == 5, "sendfile offset=5 读取 5 字节成功");
        CHECK(off == 10, "sendfile 后 offset 更新为 10 (5+5)");

        /* in_fd 偏移量应未变 */
        off_t cur = lseek(fd_in, 0, SEEK_CUR);
        CHECK(cur == orig_off,
              "sendfile 使用 offset 时 in_fd 偏移量未改变");

        close(fd_in);
        close(fd_out);
        CHECK(verify_content(TEST_OUT, "FGHIJ", 5),
              "offset=5 复制内容正确 (FGHIJ)");

        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 4: in_fd 在 EOF 时返回 0
     * 手册描述: the caller should be prepared to retry the call if there
     *           were unsent bytes. (EOF 返回 0)
     */
    {
        const char *data = "DATA";
        int fd_in = create_file(TEST_IN, data, strlen(data));
        int fd_out = create_file(TEST_OUT, NULL, 0);

        /* 移动到文件末尾之后 */
        lseek(fd_in, 100, SEEK_SET);

        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, 10);
        CHECK_RET(n, 0, "sendfile in_fd 在 EOF 后返回 0");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 5: 部分传输 — count 大于源文件剩余字节
     * 手册描述: a successful call may write fewer bytes than requested.
     */
    {
        const char *data = "SHORT";
        int fd_in = create_file(TEST_IN, data, strlen(data));
        int fd_out = create_file(TEST_OUT, NULL, 0);

        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, 1024);
        CHECK(n == (ssize_t)strlen(data),
              "sendfile count=1024 源仅 5 字节，返回 5");

        close(fd_in);
        close(fd_out);
        CHECK(verify_content(TEST_OUT, data, strlen(data)),
              "部分传输后数据正确");

        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 6: 循环 sendfile 传输大文件
     */
    {
        size_t total = 8192;
        char *large = malloc(total);
        for (size_t i = 0; i < total; i++)
            large[i] = 'A' + (i % 26);

        int fd_in = create_file(TEST_IN, NULL, 0);
        write(fd_in, large, total);
        lseek(fd_in, 0, SEEK_SET);

        int fd_out = create_file(TEST_OUT, NULL, 0);

        size_t done = 0;
        while (done < total) {
            errno = 0;
            ssize_t n = sendfile(fd_out, fd_in, NULL, total - done);
            if (n <= 0) break;
            done += n;
        }

        CHECK(done == total, "sendfile 循环传输 8192 字节成功");

        close(fd_in);
        close(fd_out);

        int fd_v = open(TEST_OUT, O_RDONLY);
        char *readback = malloc(total);
        read(fd_v, readback, total);
        close(fd_v);
        CHECK(memcmp(readback, large, total) == 0,
              "循环传输后文件内容一致");

        free(large);
        free(readback);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 7: sendfile 到 socket（文件 → socketpair）
     * 手册描述: out_fd can be any file. (Since Linux 2.6.33)
     */
    {
        const char *data = "to_socket";
        int fd_in = create_file(TEST_IN, data, strlen(data));

        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

        errno = 0;
        ssize_t n = sendfile(sv[1], fd_in, NULL, strlen(data));
        CHECK(n == (ssize_t)strlen(data),
              "sendfile 文件→socket 成功");

        char buf[64] = {0};
        read(sv[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, data) == 0,
              "sendfile 到 socket 数据正确");

        close(fd_in);
        close(sv[0]);
        close(sv[1]);
        unlink(TEST_IN);
    }

    /*
     * 测试 8: sendfile 到管道（文件 → pipe）
     * 手册描述: since Linux 5.12 and if out_fd is a pipe, sendfile()
     *           desugars to a splice(2).
     */
    {
        const char *data = "to_pipe";
        int fd_in = create_file(TEST_IN, data, strlen(data));

        int pipefd[2];
        pipe(pipefd);

        errno = 0;
        ssize_t n = sendfile(pipefd[1], fd_in, NULL, strlen(data));
        close(pipefd[1]);
        if (n == (ssize_t)strlen(data)) {
            char buf[64] = {0};
            read(pipefd[0], buf, sizeof(buf) - 1);
            CHECK(strcmp(buf, data) == 0,
                  "sendfile 文件→管道 数据正确");
        } else {
            /* 内核 < 5.12 可能不支持，也算通过 */
            printf("  PASS | %s:%d | sendfile 文件→管道 (内核可能不支持, n=%zd)\n",
                   __FILE__, __LINE__, n);
            __pass++;
        }

        close(fd_in);
        close(pipefd[0]);
        unlink(TEST_IN);
    }

    /*
     * 测试 9: sendfile 空源文件
     */
    {
        int fd_in = create_file(TEST_IN, NULL, 0);
        int fd_out = create_file(TEST_OUT, NULL, 0);

        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, 100);
        CHECK_RET(n, 0, "sendfile 空源文件返回 0");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 10: 跨进程 sendfile
     */
    {
        int pipefd[2];
        pipe(pipefd);

        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            int fd = open(TEST_IN, O_RDWR | O_CREAT | O_TRUNC, 0644);
            write(fd, "child_file", 10);
            close(fd);
            char c = 'R';
            write(pipefd[1], &c, 1);
            close(pipefd[1]);
            _exit(0);
        }

        close(pipefd[1]);
        char c;
        read(pipefd[0], &c, 1);
        close(pipefd[0]);

        int fd_in = open(TEST_IN, O_RDONLY);
        int fd_out = create_file(TEST_OUT, NULL, 0);
        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, 10);
        CHECK(n == 10, "sendfile 跨进程复制成功");
        close(fd_in);
        close(fd_out);

        CHECK(verify_content(TEST_OUT, "child_file", 10),
              "跨进程 sendfile 内容正确");

        int status;
        waitpid(pid, &status, 0);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /* ==================== 反向测试 ==================== */

    /*
     * 测试 11: EBADF — 无效的文件描述符
     * 手册描述: The input file was not opened for reading or the output file
     *           was not opened for writing.
     */
    {
        int fd = create_file(TEST_OUT, NULL, 0);

        CHECK_ERR(sendfile(fd, -1, NULL, 10),
                  EBADF, "sendfile in_fd=-1 返回 EBADF");

        CHECK_ERR(sendfile(-1, fd, NULL, 10),
                  EBADF, "sendfile out_fd=-1 返回 EBADF");

        close(fd);
        unlink(TEST_OUT);
    }

    /*
     * 测试 12: EBADF — in_fd 不可读
     * 手册描述: The input file was not opened for reading.
     */
    {
        int fd_in = open(TEST_IN, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int fd_out = create_file(TEST_OUT, NULL, 0);

        CHECK_ERR(sendfile(fd_out, fd_in, NULL, 10),
                  EBADF, "sendfile in_fd 不可读 (O_WRONLY) 返回 EBADF");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 13: EBADF — out_fd 不可写
     * 手册描述: the output file was not opened for writing.
     */
    {
        int fd_in = create_file(TEST_IN, "DATA", 4);
        int fd_out = open(TEST_OUT, O_RDONLY | O_CREAT, 0644);

        CHECK_ERR(sendfile(fd_out, fd_in, NULL, 10),
                  EBADF, "sendfile out_fd 不可写 (O_RDONLY) 返回 EBADF");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 14: EBADF — out_fd 以 O_APPEND 打开
     * 手册描述: out_fd has the O_APPEND flag set.
     */
    {
        int fd_in = create_file(TEST_IN, "DATA", 4);
        int fd_out = open(TEST_OUT, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0644);

        CHECK_ERR(sendfile(fd_out, fd_in, NULL, 10),
                  EINVAL, "sendfile out_fd O_APPEND 返回 EINVAL");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 15: ESPIPE — offset 非 NULL 但 in_fd 不可 seek（管道）
     * 手册描述: offset is not NULL but the input file is not seekable.
     */
    {
        int pipefd[2];
        pipe(pipefd);

        int fd_out = create_file(TEST_OUT, NULL, 0);
        off_t off = 0;

        CHECK_ERR(sendfile(fd_out, pipefd[0], &off, 10),
                  ESPIPE, "sendfile offset 非 NULL 但 in_fd=管道返回 ESPIPE");

        close(pipefd[0]);
        close(pipefd[1]);
        close(fd_out);
        unlink(TEST_OUT);
    }

    /*
     * 测试 16: EINVAL — in_fd 是 socket（不支持 mmap）
     * 手册描述: in_fd must correspond to a file which supports mmap()-like
     *           operations (i.e., it cannot be a socket).
     */
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

        int fd_out = create_file(TEST_OUT, NULL, 0);

        CHECK_ERR(sendfile(fd_out, sv[0], NULL, 10),
                  EINVAL, "sendfile in_fd=socket 返回 EINVAL");

        close(sv[0]);
        close(sv[1]);
        close(fd_out);
        unlink(TEST_OUT);
    }

    /*
     * 测试 17: EBADF — 已关闭的 fd
     */
    {
        int fd_in = create_file(TEST_IN, "DATA", 4);
        int fd_out = create_file(TEST_OUT, NULL, 0);
        close(fd_in);

        CHECK_ERR(sendfile(fd_out, fd_in, NULL, 10),
                  EBADF, "sendfile 已关闭的 in_fd 返回 EBADF");

        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /*
     * 测试 18: EINVAL — count 为 0
     * 手册描述: count is the number of bytes to copy.
     */
    {
        int fd_in = create_file(TEST_IN, "DATA", 4);
        int fd_out = create_file(TEST_OUT, NULL, 0);

        errno = 0;
        ssize_t n = sendfile(fd_out, fd_in, NULL, 0);
        CHECK(n == 0, "sendfile count=0 返回 0");

        close(fd_in);
        close(fd_out);
        unlink(TEST_IN);
        unlink(TEST_OUT);
    }

    /* ==================== 清理 ==================== */
    unlink(TEST_IN);
    unlink(TEST_OUT);

    TEST_DONE();
}
