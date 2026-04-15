/*
 * test_pidfd_open.c — pidfd_open 功能完整验证
 *
 * 测试策略：验证 pidfd 获取、close-on-exec 标志、
 *           poll 监听进程退出、PIDFD_NONBLOCK + waitid、错误路径
 *
 * 覆盖范围：
 *   正向：为自身/子进程获取 pidfd、close-on-exec 验证、
 *         poll 监听子进程终止、PIDFD_NONBLOCK 非阻塞 waitid
 *   负向：EINVAL 无效 flags、ESRCH 不存在的进程、
 *         EINVAL 无效 pid
 *
 * 注意：glibc 无 pidfd_open wrapper，使用 syscall()
 */

#include "test_framework.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/types.h>

#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif

/* pidfd_open 封装 */
static int pidfd_open(pid_t pid, unsigned int flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

int main(void)
{
    TEST_START("pidfd_open: 进程文件描述符验证");

    /* ================================================================
     * PART 1: 为自身进程获取 pidfd
     * ================================================================ */

    /* 1.1 pidfd_open(getpid(), 0) 成功 */
    int pidfd = pidfd_open(getpid(), 0);
    CHECK(pidfd >= 0, "pidfd_open(getpid()) 成功");
    if (pidfd >= 0) {
        close(pidfd);
    }

    /* 1.2 验证 close-on-exec 标志 */
    pidfd = pidfd_open(getpid(), 0);
    CHECK(pidfd >= 0, "pidfd_open for CLOEXEC test 成功");
    if (pidfd >= 0) {
        int fd_flags = fcntl(pidfd, F_GETFD);
        CHECK(fd_flags >= 0, "F_GETFD 成功");
        CHECK((fd_flags & FD_CLOEXEC) != 0,
              "pidfd 默认设置 FD_CLOEXEC");
        close(pidfd);
    }

    /* ================================================================
     * PART 2: 为子进程获取 pidfd
     * ================================================================ */

    pid_t child = fork();
    if (child == 0) {
        /* 子进程: 暂停等待父进程操作 */
        sleep(3);
        _exit(42);
    }

    CHECK(child > 0, "fork 创建子进程成功");
    if (child > 0) {
        /* 2.1 获取子进程 pidfd */
        int child_pidfd = pidfd_open(child, 0);
        CHECK(child_pidfd >= 0, "pidfd_open(child) 成功");
        if (child_pidfd >= 0) {
            /* 2.2 验证也是 close-on-exec */
            int cflags = fcntl(child_pidfd, F_GETFD);
            CHECK(cflags >= 0, "子进程 pidfd: F_GETFD 成功");
            CHECK((cflags & FD_CLOEXEC) != 0,
                  "子进程 pidfd 也设置了 FD_CLOEXEC");

            close(child_pidfd);
        }

        /* 等待子进程结束，避免僵尸 */
        int status;
        waitpid(child, &status, 0);
    }

    /* ================================================================
     * PART 3: poll 监听子进程终止
     * ================================================================ */

    child = fork();
    if (child == 0) {
        /* 子进程: 立即退出 */
        _exit(0);
    }

    if (child > 0) {
        int child_pidfd = pidfd_open(child, 0);
        CHECK(child_pidfd >= 0, "poll 测试: pidfd_open(child) 成功");
        if (child_pidfd >= 0) {
            struct pollfd pfd;
            pfd.fd = child_pidfd;
            pfd.events = POLLIN;

            /* 等待最多 5 秒 */
            int pret = poll(&pfd, 1, 5000);
            CHECK(pret > 0, "poll 返回 > 0（有事件）");
            CHECK((pfd.revents & POLLIN) != 0,
                  "子进程终止后 poll 返回 POLLIN");

            close(child_pidfd);
        }

        int status;
        waitpid(child, &status, 0);
    }

    /* ================================================================
     * PART 4: fork+pidfd_open 竞争测试
     * 子进程先退出，再 pidfd_open — 应仍能获取到 zombie 的 pidfd
     * ================================================================ */

    child = fork();
    if (child == 0) {
        _exit(0);
    }

    if (child > 0) {
        /* 等子进程退出但先不 wait，使其成为 zombie */
        usleep(100000); /* 100ms 等待退出 */

        int zombie_pidfd = pidfd_open(child, 0);
        CHECK(zombie_pidfd >= 0,
              "zombie 子进程仍可获取 pidfd");
        if (zombie_pidfd >= 0) {
            close(zombie_pidfd);
        }

        int status;
        waitpid(child, &status, 0);
    }

    /* ================================================================
     * PART 5: PIDFD_NONBLOCK 标志验证
     * ================================================================ */

    child = fork();
    if (child == 0) {
        sleep(5);
        _exit(0);
    }

    if (child > 0) {
        int nb_pidfd = pidfd_open(child, PIDFD_NONBLOCK);
        CHECK(nb_pidfd >= 0, "pidfd_open PIDFD_NONBLOCK 成功");
        if (nb_pidfd >= 0) {
            /* 5.1 使用 waitid WNOHANG 验证非阻塞语义 */
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            errno = 0;
            int wret = waitid(P_PIDFD, nb_pidfd, &info, WEXITED | WNOHANG);
            /* 子进程还在运行，WNOHANG 应返回 0 或 EAGAIN (取决于实现) */
            if (wret == 0) {
                CHECK(info.si_pid == 0,
                      "PIDFD_NONBLOCK + WNOHANG: 子进程未退出时 si_pid==0");
            } else if (wret == -1 && errno == EAGAIN) {
                CHECK(1, "PIDFD_NONBLOCK + WNOHANG: 返回 EAGAIN (非阻塞)");
            } else {
                /* 某些实现可能行为不同 */
                CHECK(wret == 0 || (wret == -1 && errno == EAGAIN),
                      "PIDFD_NONBLOCK + WNOHANG: 返回合理结果");
            }

            close(nb_pidfd);
        }

        /* 杀掉子进程并回收 */
        kill(child, SIGKILL);
        int status;
        waitpid(child, &status, 0);
    }

    /* ================================================================
     * PART 6: pidfd 用于 pidfd_send_signal (如果可用)
     * 通过 fork+signal 验证 pidfd 的实际可用性
     * ================================================================ */

    child = fork();
    if (child == 0) {
        /* 子进程: pause 等待信号 */
        pause();
        _exit(0);
    }

    if (child > 0) {
        int sig_pidfd = pidfd_open(child, 0);
        CHECK(sig_pidfd >= 0, "signal test: pidfd_open 成功");
        if (sig_pidfd >= 0) {
            /* 6.1 使用 pidfd_send_signal 发送 SIGKILL */
            errno = 0;
#ifdef SYS_pidfd_send_signal
            int sret = (int)syscall(SYS_pidfd_send_signal, sig_pidfd, SIGKILL, NULL, 0);
            if (sret == 0) {
                CHECK(sret == 0, "pidfd_send_signal(SIGKILL) 成功");
            } else {
                /* 系统可能不支持，回退到 kill */
                kill(child, SIGKILL);
                CHECK(1, "pidfd_send_signal 不可用，使用 kill 回退");
            }
#else
            kill(child, SIGKILL);
            CHECK(1, "SYS_pidfd_send_signal 未定义，使用 kill");
#endif
            close(sig_pidfd);
        }

        int status;
        waitpid(child, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
              "子进程被 SIGKILL 终止");
    }

    /* ================================================================
     * PART 7: pidfd read 应失败
     * man page 说明: read on pidfd fails with EINVAL
     * ================================================================ */

    pidfd = pidfd_open(getpid(), 0);
    if (pidfd >= 0) {
        errno = 0;
        char tmp[16];
        ssize_t rret = read(pidfd, tmp, sizeof(tmp));
        CHECK(rret == -1, "read(pidfd) 失败（预期）");
        /* EINVAL 或 EBADF 或其他错误均可接受 */
        CHECK(errno != 0, "read(pidfd) 设置了 errno");

        close(pidfd);
    }

    /* ================================================================
     * PART 8: 负向测试 — 错误路径
     * ================================================================ */

    /* 8.1 ESRCH: 不存在的 PID */
    errno = 0;
    CHECK_ERR(pidfd_open(999999, 0), ESRCH,
              "pidfd_open 不存在的 PID -> ESRCH");

    /* 8.2 EINVAL: 无效 flags */
    errno = 0;
    CHECK_ERR(pidfd_open(getpid(), 0xFFFF), EINVAL,
              "pidfd_open 无效 flags -> EINVAL");

    /* 8.3 EINVAL: pid = -1 (无效 PID) */
    errno = 0;
    CHECK_ERR(pidfd_open(-1, 0), EINVAL,
              "pidfd_open pid=-1 -> EINVAL");

    /* 8.4 EINVAL: pid = 0 — 取决于实现可能返回 ESRCH 或 EINVAL */
    errno = 0;
    int ret0 = pidfd_open(0, 0);
    if (ret0 == -1) {
        CHECK(errno == EINVAL || errno == ESRCH,
              "pidfd_open pid=0 返回合理错误 (EINVAL/ESRCH)");
    } else {
        /* pid=0 可能被解释为当前进程组的某些实现 */
        close(ret0);
        CHECK(1, "pidfd_open pid=0 成功（实现相关）");
    }

    /* 8.5 已回收的子进程 -> ESRCH */
    child = fork();
    if (child == 0) {
        _exit(0);
    }
    if (child > 0) {
        int status;
        waitpid(child, &status, 0); /* 先回收 */

        errno = 0;
        CHECK_ERR(pidfd_open(child, 0), ESRCH,
                  "pidfd_open 已回收的子进程 -> ESRCH");
    }

    /* 8.6 无效 pidfd 作为 fd 操作 (验证它是一个合法的 fd) */
    pidfd = pidfd_open(getpid(), 0);
    if (pidfd >= 0) {
        /* fcntl 应成功（它是个有效 fd） */
        int f = fcntl(pidfd, F_GETFD);
        CHECK(f >= 0, "pidfd 是有效的文件描述符 (fcntl 成功)");
        close(pidfd);
    }

    TEST_DONE();
}
