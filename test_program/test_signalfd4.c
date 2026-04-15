/*
 * test_signalfd4.c — signalfd4 (via signalfd)
 *
 * 正向: 创建signalfd → 屏蔽信号 → 发送信号 → read读取signalfd_siginfo → 验证ssi_signo
 *       SFD_CLOEXEC / SFD_NONBLOCK 标志验证
 *       重新绑定信号集(mask替换)
 * 负向: 无效fd / 非signalfd的fd / 无效flags / 非阻塞无信号时read
 */

#include "test_framework.h"

#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

int main(void)
{
    /* ============== 正向测试 ============== */

    /* 1. 基本: 创建 signalfd，监听 SIGUSR1 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    /* 屏蔽 SIGUSR1，防止默认处理 */
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, 0);
    CHECK(sfd >= 0, "signalfd(-1, SIGUSR1, 0) 创建成功");
    if (sfd < 0) {
        printf("  FATAL: signalfd 创建失败，终止\n");
        TEST_DONE();
    }

    /* 2. 发送 SIGUSR1 给自己，然后 read 验证 */
    raise(SIGUSR1);

    struct signalfd_siginfo fdsi;
    memset(&fdsi, 0, sizeof(fdsi));
    ssize_t rret = read(sfd, &fdsi, sizeof(fdsi));
    CHECK_RET(rret, (ssize_t)sizeof(fdsi), "read signalfd 返回 signalfd_siginfo 大小");
    CHECK(fdsi.ssi_signo == (uint32_t)SIGUSR1,
          "ssi_signo == SIGUSR1");
    /* ssi_pid 应该是当前进程 pid */
    CHECK(fdsi.ssi_pid == (uint32_t)getpid(),
          "ssi_pid == 当前进程 pid");
    close(sfd);

    /* 3. SFD_CLOEXEC: 验证 FD_CLOEXEC 标志已设置 */
    sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    CHECK(sfd >= 0, "signalfd SFD_CLOEXEC 创建成功");
    int fd_flags = fcntl(sfd, F_GETFD);
    CHECK(fd_flags >= 0 && (fd_flags & FD_CLOEXEC),
          "SFD_CLOEXEC: FD_CLOEXEC 标志已设置");
    close(sfd);

    /* 4. SFD_NONBLOCK: 无信号时 read 应返回 EAGAIN */
    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(sfd >= 0, "signalfd SFD_NONBLOCK 创建成功");
    errno = 0;
    CHECK_ERR(read(sfd, &fdsi, sizeof(fdsi)), EAGAIN,
              "非阻塞 read 无信号时应返回 EAGAIN");
    close(sfd);

    /* 5. 多信号: 监听 SIGUSR1 + SIGUSR2，发送两个信号并验证都收到 */
    sigset_t mask2;
    sigemptyset(&mask2);
    sigaddset(&mask2, SIGUSR1);
    sigaddset(&mask2, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask2, NULL);

    sfd = signalfd(-1, &mask2, 0);
    CHECK(sfd >= 0, "signalfd 多信号创建成功");

    raise(SIGUSR2);
    raise(SIGUSR1);

    /* 标准信号不保证 FIFO 顺序，收集两个信号验证集合完整性 */
    int got_usr1 = 0, got_usr2 = 0;
    for (int i = 0; i < 2; i++) {
        memset(&fdsi, 0, sizeof(fdsi));
        rret = read(sfd, &fdsi, sizeof(fdsi));
        CHECK_RET(rret, (ssize_t)sizeof(fdsi), "多信号: read 成功");
        if (fdsi.ssi_signo == (uint32_t)SIGUSR1) got_usr1 = 1;
        if (fdsi.ssi_signo == (uint32_t)SIGUSR2) got_usr2 = 1;
    }
    CHECK(got_usr1, "多信号: 收到 SIGUSR1");
    CHECK(got_usr2, "多信号: 收到 SIGUSR2");
    close(sfd);

    /* 6. 重新绑定信号集: 使用现有 sfd 替换 mask */
    sigset_t mask_only_usr2;
    sigemptyset(&mask_only_usr2);
    sigaddset(&mask_only_usr2, SIGUSR2);

    sfd = signalfd(-1, &mask2, SFD_NONBLOCK);
    CHECK(sfd >= 0, "signalfd 重新绑定: 初始创建成功");

    /* 用同一个 fd 重新绑定到只含 SIGUSR2 的 mask */
    int sfd2 = signalfd(sfd, &mask_only_usr2, 0);
    CHECK_RET(sfd2, sfd, "signalfd 重新绑定返回同一个 fd");

    /* 发送 SIGUSR1 (不在新 mask 中) 和 SIGUSR2 (在新 mask 中) */
    raise(SIGUSR1);
    raise(SIGUSR2);

    /* read 应该只能读到 SIGUSR2 */
    memset(&fdsi, 0, sizeof(fdsi));
    rret = read(sfd, &fdsi, sizeof(fdsi));
    CHECK_RET(rret, (ssize_t)sizeof(fdsi), "重新绑定: read 成功");
    CHECK(fdsi.ssi_signo == (uint32_t)SIGUSR2,
          "重新绑定: 只能读到 SIGUSR2，SIGUSR1 被过滤");
    close(sfd);

    /* 7. SIGKILL / SIGSTOP 在 mask 中应被静默忽略 (不报错) */
    sigset_t mask_kill;
    sigemptyset(&mask_kill);
    sigaddset(&mask_kill, SIGKILL);
    sigaddset(&mask_kill, SIGUSR1);
    sfd = signalfd(-1, &mask_kill, 0);
    CHECK(sfd >= 0, "signalfd 含 SIGKILL 不报错 (被静默忽略)");
    close(sfd);

    /* 8. 读取后信号被消费: read 一次后 sigpending 中不再有该信号 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(sfd >= 0, "信号消费测试: signalfd 创建成功");

    raise(SIGUSR1);
    memset(&fdsi, 0, sizeof(fdsi));
    rret = read(sfd, &fdsi, sizeof(fdsi));
    (void)rret;

    /* 再次 read 应 EAGAIN (信号已被消费) */
    errno = 0;
    CHECK_ERR(read(sfd, &fdsi, sizeof(fdsi)), EAGAIN,
              "信号消费后再次 read 应返回 EAGAIN");
    close(sfd);

    /* ============== 负向测试 ============== */

    /* 9. EBADF: 传入无效 fd (非 -1 且非有效 signalfd) */
    errno = 0;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK_ERR(signalfd(9999, &mask, 0), EBADF,
              "signalfd 无效 fd 应返回 EBADF");

    /* 10. EINVAL: 传入非 signalfd 的有效 fd (如普通文件 fd) */
    int pipe_fds[2];
    if (pipe(pipe_fds) == 0) {
        errno = 0;
        CHECK_ERR(signalfd(pipe_fds[0], &mask, 0), EINVAL,
                  "signalfd 传入 pipe fd 应返回 EINVAL");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }

    /* 11. EINVAL: 无效 flags */
    errno = 0;
    CHECK_ERR(signalfd(-1, &mask, 0xBAD), EINVAL,
              "signalfd 无效 flags 应返回 EINVAL");

    /* 12. read 缓冲区过小: 小于 sizeof(signalfd_siginfo) */
    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    raise(SIGUSR1);
    errno = 0;
    {
        char small_buf[8];
        CHECK_ERR(read(sfd, small_buf, sizeof(small_buf)), EINVAL,
                  "read signalfd 缓冲区过小应返回 EINVAL");
    }
    /* 消费残留的 pending 信号 (EINVAL 的 read 不消耗信号) */
    {
        struct signalfd_siginfo tmp;
        ssize_t tmp_r = read(sfd, &tmp, sizeof(tmp));
        (void)tmp_r;
    }
    close(sfd);

    /* 13. 信号未被屏蔽时仍可创建 signalfd (非错误) */
    sigset_t mask_usr2;
    sigemptyset(&mask_usr2);
    sigaddset(&mask_usr2, SIGUSR2);
    /* SIGUSR2 未被屏蔽 */
    sfd = signalfd(-1, &mask_usr2, SFD_NONBLOCK);
    CHECK(sfd >= 0, "信号未屏蔽时 signalfd 仍可创建");
    /* 但 read 可能不会收到该信号 (因为信号不会被排队到 signalfd) */
    close(sfd);

    /* 14. close 已关闭的 fd (EBADF) */
    sfd = signalfd(-1, &mask, 0);
    close(sfd);
    errno = 0;
    CHECK_ERR(close(sfd), EBADF,
              "close 已关闭的 signalfd 应返回 EBADF");

    /* 信号屏蔽无需解除: 进程即将退出 */

    TEST_DONE();
}
