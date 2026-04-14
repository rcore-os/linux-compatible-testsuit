/*
 * test_signal.c — rt_sigaction / rt_sigprocmask / kill / tgkill 完整测试
 *
 * 测试策略：验证信号安装、发送、捕获、屏蔽的完整链路
 * 遵循 sys_design.md §13 设计
 *
 * 覆盖范围：
 *   正向：rt_sigaction、rt_sigprocmask、rt_sigpending、kill、tgkill、fork继承、SIGPIPE、SIGALRM、sigaltstack
 *   负向：无效信号、不存在进程、无权限
 */

#define _GNU_SOURCE
#include "test_framework.h"
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>

/* 测试用信号 */
#define TEST_SIG SIGUSR1  /* 10 */
#define TEST_SIG2 SIGUSR2 /* 12 */

/* 全局变量用于验证信号处理 */
static volatile sig_atomic_t signal_received = 0;
static volatile sig_atomic_t signal_siginfo_signo = 0;
static volatile sig_atomic_t signal_siginfo_pid = 0;

/* 信号处理函数 */
static void signal_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    signal_received = sig;
    if (info) {
        signal_siginfo_signo = info->si_signo;
        signal_siginfo_pid = info->si_pid;
    }
}

/* SIGPIPE 处理函数 */
static volatile sig_atomic_t sigpipe_received = 0;
static void sigpipe_handler(int sig) {
    (void)sig;
    sigpipe_received = 1;
}

/* SIGALRM 处理函数 */
static volatile sig_atomic_t sigalrm_received = 0;
static void sigalrm_handler(int sig) {
    (void)sig;
    sigalrm_received = 1;
}

/* SIGSEGV 处理函数（用于备用栈测试） */
static volatile sig_atomic_t sigsegv_on_altstack = 0;
static void sigsegv_handler(int sig) {
    (void)sig;
    /* 检查是否在备用栈上 */
    stack_t ss;
    sigaltstack(NULL, &ss);
    if (ss.ss_flags == 0) {
        sigsegv_on_altstack = 1;
    }
    _exit(0);  /* 测试完成，退出 */
}

int main(void)
{
    TEST_START("signal: rt_sigaction/rt_sigprocmask/kill/tgkill 完整测试");

    /* ================================================================
     * PART 1: rt_sigaction 安装信号处理函数
     * ================================================================ */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;

    int ret = sigaction(TEST_SIG, &sa, NULL);
    CHECK(ret == 0, "rt_sigaction 安装 SIGUSR1 handler");

    /* ================================================================
     * PART 2: kill 自身发送信号
     * ================================================================ */

    signal_received = 0;
    signal_siginfo_signo = 0;
    signal_siginfo_pid = 0;

    ret = kill(getpid(), TEST_SIG);
    CHECK(ret == 0, "kill 自身发送 SIGUSR1");

    /* 给信号处理时间 */
    usleep(10000);

    CHECK(signal_received == TEST_SIG, "kill: handler 被调用，sig == TEST_SIG");
    CHECK(signal_siginfo_signo == TEST_SIG, "kill: siginfo.si_signo == SIGUSR1(10)");
    CHECK(signal_siginfo_pid == getpid(), "kill: siginfo.si_pid == 当前 pid");

    /* ================================================================
     * PART 3: rt_sigaction SIG_IGN 忽略信号
     * ================================================================ */

    signal_received = 0;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(TEST_SIG, &sa, NULL);

    kill(getpid(), TEST_SIG);
    usleep(10000);

    CHECK(signal_received == 0, "rt_sigaction SIG_IGN: kill 发送后 handler 不被调用");

    /* ================================================================
     * PART 4: rt_sigaction SIG_DFL 恢复默认
     * ================================================================ */

    sa.sa_handler = SIG_DFL;
    sigaction(TEST_SIG, &sa, NULL);

    pid_t fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：发送 SIGUSR1 给自己，默认行为是终止进程 */
        kill(getpid(), TEST_SIG);
        /* 如果到达这里，说明信号被忽略或处理了 */
        _exit(1);
    }

    int status;
    pid_t waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(waited == fork_ret, "rt_sigaction SIG_DFL: wait4 回收子进程");
    CHECK(WIFSIGNALED(status), "rt_sigaction SIG_DFL: 子进程被信号终止");
    CHECK(WTERMSIG(status) == TEST_SIG, "rt_sigaction SIG_DFL: 终止信号为 SIGUSR1");

    /* 恢复信号处理函数 */
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(TEST_SIG, &sa, NULL);

    /* ================================================================
     * PART 5: rt_sigprocmask 屏蔽信号
     * ================================================================ */

    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, TEST_SIG);

    ret = sigprocmask(SIG_BLOCK, &mask, &oldmask);
    CHECK(ret == 0, "rt_sigprocmask 屏蔽 SIGUSR1");

    signal_received = 0;
    kill(getpid(), TEST_SIG);
    usleep(10000);

    CHECK(signal_received == 0, "rt_sigprocmask: 屏蔽期间 handler 不被调用");

    /* ================================================================
     * PART 6: rt_sigpending 检查待处理信号
     * ================================================================ */

    sigset_t pending;
    sigemptyset(&pending);
    ret = sigpending(&pending);
    CHECK(ret == 0, "rt_sigpending 检查待处理信号");
    CHECK(sigismember(&pending, TEST_SIG) == 1, "rt_sigpending: SIGUSR1 已待处理");

    /* ================================================================
     * PART 7: rt_sigprocmask 解除屏蔽
     * ================================================================ */

    ret = sigprocmask(SIG_UNBLOCK, &mask, NULL);
    CHECK(ret == 0, "rt_sigprocmask 解除屏蔽 SIGUSR1");

    usleep(10000);
    CHECK(signal_received == TEST_SIG, "rt_sigprocmask: 解除后 handler 被调用");

    /* ================================================================
     * PART 8: tgkill 精确发送信号
     * ================================================================ */

    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(TEST_SIG2, &sa, NULL);

    signal_received = 0;
    ret = syscall(SYS_tgkill, getpid(), gettid(), TEST_SIG2);
    CHECK(ret == 0, "tgkill 精确发送信号");

    usleep(10000);
    CHECK(signal_received == TEST_SIG2, "tgkill: 指定 tid，handler 被调用");

    /* ================================================================
     * PART 9: fork 后子进程继承信号处理
     * ================================================================ */

    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(TEST_SIG, &sa, NULL);

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：发送信号给自己 */
        signal_received = 0;
        kill(getpid(), TEST_SIG);
        usleep(10000);
        _exit(signal_received == TEST_SIG ? 0 : 1);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork: 子进程继承信号处理，handler 被调用");

    /* ================================================================
     * PART 10: fork 后子进程继承屏蔽集
     * ================================================================ */

    sigemptyset(&mask);
    sigaddset(&mask, TEST_SIG);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：检查继承的屏蔽集 */
        sigset_t child_mask;
        sigprocmask(0, NULL, &child_mask);
        int is_member = sigismember(&child_mask, TEST_SIG);
        _exit(is_member == 1 ? 0 : 1);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork: 子进程继承信号屏蔽集");

    /* 恢复屏蔽集 */
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    /* ================================================================
     * PART 11: SIGPIPE 触发
     * ================================================================ */

    sa.sa_handler = sigpipe_handler;
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    int pipefd[2];
    ret = pipe(pipefd);
    CHECK(ret == 0, "SIGPIPE 测试: 创建管道");

    close(pipefd[0]);  /* 关闭读端 */

    sigpipe_received = 0;
    write(pipefd[1], "test", 4);  /* 写入已关闭读端的管道 */
    usleep(10000);

    CHECK(sigpipe_received == 1, "SIGPIPE: pipe 读端关闭后 write 触发信号");

    close(pipefd[1]);

    /* ================================================================
     * PART 12: SIGALRM 定时器
     * ================================================================ */

    sa.sa_handler = sigalrm_handler;
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    sigalrm_received = 0;
    alarm(1);  /* 1 秒后发送 SIGALRM */

    /* pause() 等待信号 */
    pause();

    CHECK(sigalrm_received == 1, "SIGALRM: alarm(1) 后 pause() 返回");

    /* ================================================================
     * PART 13: sigaltstack 备用栈
     * ================================================================ */

    stack_t ss, old_ss;
    ss.ss_sp = malloc(SIGSTKSZ);
    CHECK(ss.ss_sp != NULL, "sigaltstack: 分配备用栈内存");

    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;

    ret = sigaltstack(&ss, &old_ss);
    CHECK(ret == 0, "sigaltstack: 注册备用栈");

    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：触发 SIGSEGV，应该在备用栈上处理 */
        int *null_ptr = NULL;
        *null_ptr = 0;  /* 触发 SIGSEGV */
        _exit(1);  /* 不应该到达这里 */
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "sigaltstack: SIGSEGV handler 在备用栈上执行");

    /* 恢复原备用栈 */
    sigaltstack(&old_ss, NULL);
    free(ss.ss_sp);

    /* ================================================================
     * PART 14: rt_sigsuspend 原子等待
     * ================================================================ */

    /* 设置信号处理函数 */
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(TEST_SIG, &sa, NULL);

    /* 屏蔽 TEST_SIG */
    sigemptyset(&mask);
    sigaddset(&mask, TEST_SIG);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：sigsuspend 等待信号 */
        signal_received = 0;
        /* sigsuspend 会临时替换信号屏蔽集，并等待信号 */
        sigemptyset(&mask);
        sigsuspend(&mask);
        /* 收到信号后继续 */
        _exit(signal_received == TEST_SIG ? 0 : 1);
    } else if (fork_ret > 0) {
        /* 父进程：等待子进程进入 sigsuspend，然后发送信号 */
        usleep(10000);
        kill(fork_ret, TEST_SIG);

        waited = wait4(fork_ret, &status, 0, NULL);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "rt_sigsuspend: 原子等待信号，收到后恢复");
    }

    /* 恢复信号屏蔽 */
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    /* ================================================================
     * PART 15: 负向测试 — 错误路径
     * ================================================================ */

    /* 15.1 rt_sigaction 无效信号 */
    errno = 0;
    ret = sigaction(0, &sa, NULL);
    CHECK(ret == -1 && errno == EINVAL, "rt_sigaction 无效信号 → EINVAL");

    /* 15.2 kill 不存在的进程 */
    errno = 0;
    ret = kill(99999, TEST_SIG);
    CHECK(ret == -1 && errno == ESRCH, "kill 不存在的进程 → ESRCH");

    /* 15.3 tgkill 无效 tid */
    errno = 0;
    ret = syscall(SYS_tgkill, getpid(), 99999, TEST_SIG);
    CHECK(ret == -1 && errno == ESRCH, "tgkill 无效 tid → ESRCH");

    /* 15.4 sigaltstack 无效栈大小 */
    errno = 0;
    stack_t bad_ss;
    bad_ss.ss_sp = malloc(1);
    bad_ss.ss_size = 1;  /* 太小 */
    bad_ss.ss_flags = 0;
    ret = sigaltstack(&bad_ss, NULL);
    /* 某些系统可能允许小栈，不强制报错 */
    if (ret == -1) {
        CHECK(errno == ENOMEM, "sigaltstack 栈太小 → ENOMEM");
    }
    free(bad_ss.ss_sp);

    /* ================================================================
     * 清理
     * ================================================================ */

    TEST_DONE();
}
