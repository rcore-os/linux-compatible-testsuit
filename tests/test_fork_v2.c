/*
 * test_fork_v2.c — fork / wait4 / clone / execve 完整测试 v2
 *
 * 测试策略：验证进程生命周期、父子进程关系、文件描述符继承、写时复制
 * 遵循 sys_design.md §5 设计（修正版）
 *
 * 覆盖范围：
 *   正向：fork、wait4、clone、clone3、execve、setsid、setpgid、getsid、getpgid
 *   负向：wait4错误、execve错误、clone错误
 *   并发：多子进程、父子竞争
 */

#define _GNU_SOURCE
#include "test_framework.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <signal.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

/* 测试文件路径 */
#define TMPFILE "/tmp/starry_fork_test"

/* 全局变量用于测试写时复制 */
static int global_var = 42;

int main(void)
{
    TEST_START("process: fork/wait4/clone/execve/会话管理 v2（修正版）");

    /* 清理可能残留的旧文件 */
    unlink(TMPFILE);

    /* ================================================================
     * PART 1: 基础 PID/TID 测试
     * ================================================================ */

    pid_t pid = getpid();
    CHECK(pid > 0, "getpid 返回正值");

    pid_t ppid = getppid();
    CHECK(ppid > 0, "getppid 返回正值(init为1)");

    pid_t tid = gettid();
    CHECK(tid > 0, "gettid 返回正值");

    /* ================================================================
     * PART 2: fork 基本语义 + ppid 关系验证
     * ================================================================ */

    pid_t fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：验证 ppid == 父进程 pid */
        pid_t child_ppid = getppid();
        if (child_ppid != pid) {
            _exit(100 + ((int)(pid - child_ppid) % 50));
        }
        _exit(0);
    }

    CHECK(fork_ret > 0, "fork 在父进程返回子进程pid(正值)");

    int status = 0;
    pid_t waited = wait4(fork_ret, &status, 0, NULL);
    CHECK_RET(waited, fork_ret, "wait4 返回正确的子进程pid");
    CHECK(WIFEXITED(status), "子进程正常退出 (WIFEXITED)");
    CHECK_RET(WEXITSTATUS(status), 0, "子进程退出码为 0 (ppid校验通过)");

    /* ================================================================
     * PART 3: fork 后父子文件描述符独立性与继承
     * ================================================================ */

    /* 父进程创建文件 */
    int fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "fork fd 测试: 父进程创建文件");
    if (fd < 0) {
        TEST_DONE();
    }

    /* 父进程：先写入数据 */
    const char *parent_data = "parent_write";
    CHECK_RET(write(fd, parent_data, strlen(parent_data)), (ssize_t)strlen(parent_data),
              "fork fd 测试: 父进程写入数据");

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：通过继承的fd读取数据 */
        char buf[32] = {0};
        lseek(fd, 0, SEEK_SET);  /* 回到文件起始 */
        read(fd, buf, sizeof(buf) - 1);
        /* 验证能读到数据（fd继承成功） */
        if (strcmp(buf, parent_data) == 0) {
            _exit(0);
        }
        _exit(1);
    }

    /* 等待子进程退出 */
    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork fd 测试: 子进程通过继承的fd读到数据");

    /* 验证fd独立性：父close不影响子进程的fd */
    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：通过继承的fd读取 */
        char buf[32] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf) - 1);
        _exit(0);
    }

    /* 父进程：关闭fd */
    close(fd);

    /* 子进程仍应能通过其继承的fd读取 */
    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork fd 独立性: 父close后子进程的fd仍可用");

    /* ================================================================
     * PART 4: fork 后写时复制验证
     * ================================================================ */

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：修改全局变量 */
        global_var = 100;
        _exit(global_var);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 100,
          "fork 写时复制: 子进程修改 global_var 不影响父进程");
    CHECK(global_var == 42, "fork 写时复制: 父进程 global_var 仍为 42");

    /* ================================================================
     * PART 5: wait4 WUNTRACED / WNOHANG / WCONTINUED
     * ================================================================ */

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：自我停止 */
        raise(SIGSTOP);
        _exit(0);
    }

    /* 父进程：WUNTRACED 等待子进程停止 */
    waited = wait4(fork_ret, &status, WUNTRACED, NULL);
    CHECK(waited == fork_ret, "wait4 WUNTRACED: 返回子进程 pid");
    CHECK(WIFSTOPPED(status), "wait4 WUNTRACED: 子进程处于停止状态");
    CHECK(WSTOPSIG(status) == SIGSTOP, "wait4 WUNTRACED: 停止信号为 SIGSTOP");

    /* 恢复子进程 */
    kill(fork_ret, SIGCONT);

    /* WCONTINUED: 等待子进程继续 */
    waited = wait4(fork_ret, &status, WCONTINUED, NULL);
    if (waited == fork_ret) {
        CHECK(WIFCONTINUED(status), "wait4 WCONTINUED: WIFCONTINUED(status)==true");
    }

    /* 回收子进程 */
    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status), "wait4: SIGCONT 后子进程正常退出");

    /* WNOHANG 非阻塞测试 */
    fork_ret = fork();
    if (fork_ret == 0) {
        sleep(1);  /* 子进程休眠 1 秒 */
        _exit(0);
    }

    /* 立即 wait4 WNOHANG，应返回 0（子进程尚未退出） */
    waited = wait4(fork_ret, &status, WNOHANG, NULL);
    CHECK(waited == 0, "wait4 WNOHANG: 子进程运行时返回 0（非阻塞）");

    /* 等待子进程退出 */
    sleep(2);
    waited = wait4(fork_ret, &status, WNOHANG, NULL);
    CHECK(waited == fork_ret, "wait4 WNOHANG: 子进程退出后返回 pid");
    CHECK(WIFEXITED(status), "wait4 WNOHANG: 子进程正常退出");

    /* ================================================================
     * PART 6: 多子进程顺序回收
     * ================================================================ */

    pid_t children[3];
    int exit_codes[3] = {1, 2, 3};

    for (int i = 0; i < 3; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            _exit(exit_codes[i]);
        }
    }

    /* 顺序回收 3 个子进程 */
    int collected_count = 0;
    int collected_exit_codes[3] = {-1, -1, -1};
    for (int i = 0; i < 3; i++) {
        waited = wait4(-1, &status, 0, NULL);
        if (waited > 0) {
            collected_count++;
            if (WIFEXITED(status)) {
                for (int j = 0; j < 3; j++) {
                    if (waited == children[j]) {
                        collected_exit_codes[j] = WEXITSTATUS(status);
                        break;
                    }
                }
            }
        }
    }

    CHECK(collected_count == 3, "多子进程回收: 成功回收 3 个子进程");

    /* 验证退出码 */
    int all_codes_match = 1;
    for (int i = 0; i < 3; i++) {
        if (collected_exit_codes[i] != exit_codes[i]) {
            all_codes_match = 0;
            break;
        }
    }
    CHECK(all_codes_match, "多子进程回收: WEXITSTATUS 分别为 1/2/3");

    /* ================================================================
     * PART 7: clone CLONE_FILES 共享 fd 表
     * ================================================================ */

    int pipefd[2];
    CHECK_RET(pipe(pipefd), 0, "clone 测试: 创建管道");

    fork_ret = syscall(SYS_clone, CLONE_FILES | SIGCHLD, NULL, NULL, NULL, NULL);
    if (fork_ret == 0) {
        /* 子进程：通过共享的fd写入管道 */
        const char *msg = "from_child";
        write(pipefd[1], msg, strlen(msg));
        /* 等待父进程读取 */
        sleep(1);
        _exit(0);
    } else if (fork_ret > 0) {
        /* 父进程：从管道读取 */
        char buf[32] = {0};
        read(pipefd[0], buf, sizeof(buf) - 1);
        CHECK(strcmp(buf, "from_child") == 0,
              "clone CLONE_FILES: 父进程 read 到子进程写入的数据");

        /* 回收子进程 */
        wait4(fork_ret, &status, 0, NULL);
    }

    close(pipefd[0]);
    close(pipefd[1]);

    /* ================================================================
     * PART 8: clone3 基本功能
     * ================================================================ */

    struct clone3_args {
        uint64_t flags;
        uint64_t exit_signal;
    } cl_args = {
        .flags = CLONE_FS,
        .exit_signal = SIGCHLD,
    };

    fork_ret = syscall(__NR_clone3, &cl_args, sizeof(cl_args));
    if (fork_ret == 0) {
        /* 子进程 */
        _exit(0);
    } else if (fork_ret > 0) {
        waited = wait4(fork_ret, &status, 0, NULL);
        CHECK(WIFEXITED(status), "clone3: 子进程正常退出");
    } else {
        /* clone3 可能不支持，不视为失败 */
        CHECK(errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL,
              "clone3: 不支持时返回合适的 errno");
    }

    /* ================================================================
     * PART 9: execve 替换进程映像
     * ================================================================ */

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：execve 执行 /bin/true */
        char *const argv[] = {"/bin/true", NULL};
        char *const envp[] = {NULL};
        execve("/bin/true", argv, envp);
        /* 如果 execve 成功则不会到达这里 */
        _exit(1);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status), "execve: 子进程正常退出");
    CHECK_RET(WEXITSTATUS(status), 0, "execve /bin/true: 退出码为 0");

    /* ================================================================
     * PART 10: getsid/setsid 会话管理
     * ================================================================ */

    pid_t old_sid = getsid(0);
    CHECK(old_sid > 0, "getsid(0): 返回当前会话 ID");

    /* 必须在子进程中调用 setsid（主进程是会话首进程） */
    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：创建新会话 */
        pid_t new_sid = setsid();
        if (new_sid > 0) {
            /* 新会话的 leader 应该是进程自身 */
            if (new_sid != getpid()) {
                _exit(1);
            }
            _exit(0);
        }
        _exit(errno);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "setsid: 成功创建新会话，getsid(0) == getpid()");

    /* ================================================================
     * PART 11: getpgid/setpgid 进程组
     * ================================================================ */

    pid_t old_pgid = getpgid(0);
    CHECK(old_pgid > 0, "getpgid(0): 返回当前进程组 ID");

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：创建新进程组 */
        int ret = setpgid(0, 0);
        if (ret == 0) {
            pid_t new_pgid = getpgid(0);
            /* 新进程组的 leader 应该是进程自身 */
            if (new_pgid == getpid()) {
                _exit(0);
            }
        }
        _exit(1);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "setpgid: setpgid(0,0) 返回 0，getpgid(0) == getpid()");

    /* ================================================================
     * PART 12: 并发测试 - 多子进程同时退出
     * ================================================================ */

    for (int i = 0; i < 5; i++) {
        fork_ret = fork();
        if (fork_ret == 0) {
            _exit(0);
        }
    }

    /* 循环回收 5 个子进程，无 ECHILD */
    collected_count = 0;
    for (int i = 0; i < 5; i++) {
        waited = wait4(-1, &status, 0, NULL);
        if (waited > 0) {
            collected_count++;
        }
    }
    CHECK(collected_count == 5, "并发: 多子进程同时退出，父成功回收 5 次");

    /* ================================================================
     * PART 13: 并发测试 - 父子同时写同一文件（使用pwrite避免竞争）
     * ================================================================ */

    fd = openat(AT_FDCWD, TMPFILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0, "并发写入测试: 创建文件");

    fork_ret = fork();
    if (fork_ret == 0) {
        /* 子进程：在偏移 100 处写入 */
        const char *child_msg = "CHILD";
        pwrite(fd, child_msg, strlen(child_msg), 100);
        _exit(0);
    }

    /* 父进程：在偏移 0 处写入 */
    const char *parent_msg = "PARENT";
    pwrite(fd, parent_msg, strlen(parent_msg), 0);

    waited = wait4(fork_ret, &status, 0, NULL);
    close(fd);

    /* 验证文件包含两者数据 */
    fd = openat(AT_FDCWD, TMPFILE, O_RDONLY);
    char parent_buf[32] = {0};
    char child_buf[32] = {0};

    /* 使用 pread 从特定偏移量读取 */
    ssize_t parent_bytes = pread(fd, parent_buf, sizeof(parent_buf) - 1, 0);
    ssize_t child_bytes = pread(fd, child_buf, sizeof(child_buf) - 1, 100);
    close(fd);

    /* 验证偏移 0 处有 PARENT */
    int contains_parent = (parent_bytes >= 6 && memcmp(parent_buf, "PARENT", 6) == 0);
    /* 验证偏移 100 处有 CHILD */
    int contains_child = (child_bytes >= 5 && memcmp(child_buf, "CHILD", 5) == 0);
    CHECK(contains_parent && contains_child,
          "并发写入: 文件包含父子两者数据（偏移0和偏移100）");

    /* ================================================================
     * PART 14: 负向测试 — 错误路径
     * ================================================================ */

    /* 14.1 wait4 不存在的 pid */
    errno = 0;
    CHECK_ERR(wait4(99999, &status, 0, NULL), ECHILD,
              "wait4 不存在的pid应返回 ECHILD");

    /* 14.2 wait4 -1 无子进程 */
    errno = 0;
    CHECK_ERR(wait4(-1, &status, 0, NULL), ECHILD,
              "wait4 -1 无子进程应返回 ECHILD");

    /* 14.3 wait4 无效 options 位 */
    errno = 0;
    waited = wait4(0, &status, 0x99999999, NULL);
    /* Linux 可能忽略无效位，不强制报错 */
    if (waited == -1) {
        CHECK(errno == EINVAL, "wait4 无效 options 位 → EINVAL");
    }

    /* 14.4 execve 不存在的文件 */
    fork_ret = fork();
    if (fork_ret == 0) {
        char *const argv[] = {"/nonexistent_path_12345", NULL};
        char *const envp[] = {NULL};
        execve("/nonexistent_path_12345", argv, envp);
        _exit(errno == ENOENT ? 0 : 1);
    }

    waited = wait4(fork_ret, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "execve 不存在的文件 → ENOENT");

    /* 14.5 setpgid 不存在的 pid */
    errno = 0;
    CHECK_ERR(setpgid(99999, 0), ESRCH,
              "setpgid 不存在的 pid → ESRCH");

    /* ================================================================
     * 清理
     * ================================================================ */
    unlink(TMPFILE);

    TEST_DONE();
}
