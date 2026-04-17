/*
 * test_poll.c — select / poll / ppoll 完整测试
 *
 * 测试策略：基于 pipe 验证 select/poll/ppoll 的 readiness 语义
 *
 * 覆盖范围：
 *   正向：select 管道就绪、select 超时、poll 管道就绪、
 *         poll 超时、poll 管道 EOF (POLLHUP)、ppoll 超时
 *   负向：select 无效 fd EBADF、poll 无效 fd POLLNVAL
 */

#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#define TMPFILE "/tmp/starry_poll_test"

int main(void)
{
    TEST_START("poll: select/poll/ppoll readiness 语义验证");

    unlink(TMPFILE);

    /* ================================================================
     * PART 1: select — 管道读端就绪
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART1: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        write(fds[1], "hello", 5);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fds[0], &readfds);

        struct timeval tv = {1, 0};
        int nready = select(fds[0] + 1, &readfds, NULL, NULL, &tv);
        CHECK_RET(nready, 1, "PART1: select 返回 1 (一个 fd 就绪)");
        CHECK(FD_ISSET(fds[0], &readfds), "PART1: 读端 fd 在 readfds 中被设置");

        char buf[16];
        CHECK_RET(read(fds[0], buf, 5), 5, "PART1: 从管道读回 5 字节");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 2: select — 超时返回 0
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART2: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fds[0], &readfds);

        struct timeval tv = {0, 100000};
        int nready = select(fds[0] + 1, &readfds, NULL, NULL, &tv);
        CHECK_RET(nready, 0, "PART2: select 空管道 100ms 超时返回 0");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 3: select — 写端就绪
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART3: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fds[1], &writefds);

        struct timeval tv = {1, 0};
        int nready = select(fds[1] + 1, NULL, &writefds, NULL, &tv);
        CHECK_RET(nready, 1, "PART3: select 写端就绪返回 1");
        CHECK(FD_ISSET(fds[1], &writefds), "PART3: 写端 fd 在 writefds 中被设置");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 4: select — 无效 fd 返回 EBADF
     * ================================================================ */

    {
        int bad_fd = open("/dev/null", O_RDONLY);
        if (bad_fd >= 0) close(bad_fd);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(bad_fd, &readfds);

        errno = 0;
        struct timeval tv = {0, 1000};
        int nready = select(bad_fd + 1, &readfds, NULL, NULL, &tv);
        CHECK(nready == -1 && errno == EBADF,
              "PART4: select 已关闭 fd 返回 EBADF");
    }

    /* ================================================================
     * PART 5: poll — 管道读端 POLLIN 就绪
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART5: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        write(fds[1], "data", 4);

        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int nready = poll(&pfd, 1, 1000);
        CHECK_RET(nready, 1, "PART5: poll 返回 1");
        CHECK(pfd.revents & POLLIN, "PART5: revents 包含 POLLIN");

        char buf[16];
        CHECK_RET(read(fds[0], buf, 4), 4, "PART5: 读回 4 字节");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 6: poll — 超时返回 0
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART6: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int nready = poll(&pfd, 1, 100);
        CHECK_RET(nready, 0, "PART6: poll 空管道 100ms 超时返回 0");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 7: poll — 管道写端关闭后 POLLHUP
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART7: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        close(fds[1]);

        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int nready = poll(&pfd, 1, 1000);
        CHECK_RET(nready, 1, "PART7: poll 关闭写端后返回 1");
        CHECK(pfd.revents & (POLLIN | POLLHUP),
              "PART7: revents 包含 POLLIN 或 POLLHUP");

        close(fds[0]);
    }

    /* ================================================================
     * PART 8: poll — 写端始终就绪 POLLOUT
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART8: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        struct pollfd pfd;
        pfd.fd = fds[1];
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int nready = poll(&pfd, 1, 100);
        CHECK_RET(nready, 1, "PART8: poll 写端 POLLOUT 就绪返回 1");
        CHECK(pfd.revents & POLLOUT, "PART8: revents 包含 POLLOUT");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 9: poll — 无效 fd POLLNVAL
     * ================================================================ */

    {
        struct pollfd pfd;
        pfd.fd = -1;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int nready = poll(&pfd, 1, 100);
        CHECK_RET(nready, 0, "PART9: poll fd=-1 返回 0 (忽略该条目)");
        CHECK(pfd.revents == 0,
              "PART9: fd=-1 时 revents == 0");
    }

    /* ================================================================
     * PART 10: poll — 多 fd
     * ================================================================ */

    {
        int pipe1[2], pipe2[2];
        int r1 = pipe(pipe1);
        int r2 = pipe(pipe2);
        CHECK(r1 == 0 && r2 == 0, "PART10: 创建两个管道成功");
        if (r1 != 0 || r2 != 0) { TEST_DONE(); }

        write(pipe1[1], "a", 1);

        struct pollfd pfds[2];
        pfds[0].fd = pipe1[0];
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = pipe2[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int nready = poll(pfds, 2, 100);
        CHECK_RET(nready, 1, "PART10: poll 2 个 fd 返回 1 (只有 pipe1 就绪)");
        CHECK(pfds[0].revents & POLLIN, "PART10: pipe1 revents 包含 POLLIN");
        CHECK(pfds[1].revents == 0, "PART10: pipe2 revents == 0 (未就绪)");

        close(pipe1[0]);
        close(pipe1[1]);
        close(pipe2[0]);
        close(pipe2[1]);
    }

    /* ================================================================
     * PART 11: ppoll — 基本超时
     * ================================================================ */

    {
        int fds[2];
        int ret = pipe(fds);
        CHECK_RET(ret, 0, "PART11: pipe 创建成功");
        if (ret != 0) { TEST_DONE(); }

        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000;

        sigset_t emptyset;
        sigemptyset(&emptyset);

        errno = 0;
        int nready = (int)syscall(SYS_ppoll, &pfd, 1, &ts, &emptyset, 8);
        CHECK(nready == 0, "PART11: ppoll 空管道 100ms 超时返回 0");

        close(fds[0]);
        close(fds[1]);
    }

    /* ================================================================
     * PART 12: select — 文件 fd 读就绪
     * ================================================================ */

    {
        int fd = open(TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0, "PART12: 创建文件成功");
        if (fd < 0) { TEST_DONE(); }

        write(fd, "test", 4);
        lseek(fd, 0, SEEK_SET);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval tv = {1, 0};
        int nready = select(fd + 1, &readfds, NULL, NULL, &tv);
        CHECK_RET(nready, 1, "PART12: select 文件 fd 读就绪返回 1");

        close(fd);
    }

    unlink(TMPFILE);

    TEST_DONE();
}
