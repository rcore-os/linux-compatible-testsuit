/*
 * test_sigstop.c - SIGSTOP 应暂停进程而非杀死
 *
 * 对应 bug: StarryOS SignalOSAction::Stop 调用 do_exit 杀死进程
 * 对应 PR:  https://github.com/rcore-os/tgoskits/pull/202
 *
 * 编译: riscv64-linux-musl-gcc -static -o test_sigstop test_sigstop.c
 */

#include "test_framework.h"
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    TEST_START("SIGSTOP/SIGCONT: job control");

    pid_t pid = fork();
    CHECK(pid >= 0, "fork succeeds");

    if (pid == 0) {
        /* 子进程：等待信号 */
        while (1) pause();
        _exit(0);
    }

    usleep(50000); /* 让子进程启动 */

    /* 发送 SIGSTOP */
    CHECK(kill(pid, SIGSTOP) == 0, "send SIGSTOP");

    /* waitpid 应报告子进程被暂停 */
    int status;
    pid_t w = waitpid(pid, &status, WUNTRACED);
    CHECK(w == pid, "waitpid returns child pid");
    CHECK(WIFSTOPPED(status), "child is stopped (not killed)");
    if (WIFSTOPPED(status)) {
        CHECK(WSTOPSIG(status) == SIGSTOP, "stop signal is SIGSTOP");
    }

    /* 发送 SIGCONT 恢复 */
    CHECK(kill(pid, SIGCONT) == 0, "send SIGCONT");
    usleep(50000);

    /* 终止子进程 */
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status), "child terminated by SIGTERM after resume");

    TEST_DONE();
}
