# StarryOS 系统调用测试优先级（基于 tgoskits 最新版本）

## 目标

基于 `tgoskits` 仓库中 StarryOS 内核的 **实际 syscall 分发表**（`os/StarryOS/kernel/src/syscall/mod.rs`），在原始 `syscall_priority.md` 基础上补充遗漏的 syscall 组。

原始文档遗漏的原因：

1. **原始文档面向 `linux-compatible-testsuit` 仓库**，是通用 Linux 兼容性测试规划，未考虑 StarryOS 已实现的 prctl、getrlimit、sched_affinity、fsync 族等。
2. **分类粒度不足**：原始"系统信息与身份"组仅列 getuid/getgid，遗漏了 setresuid/setgroups 等写操作；"核心信号"组仅列 sigaction/sigprocmask/kill，遗漏了 sigaltstack/rt_sigtimedwait 等。
3. **版本差异**：StarryOS 持续迭代，部分 syscall 是优先级规划之后才实现的。

---

## 一、评分模型

与原始文档完全一致：

| 维度 | 权重 | 含义 |
|------|------|------|
| 通用性 (U) | 35% | 典型 Linux 用户态程序使用范围有多广 |
| 重要性 (I) | 35% | 出 bug 后对功能正确性、安全性、稳定性的影响有多大 |
| 基础性 (B) | 20% | 是否是其他测试或其他子系统的依赖前提 |
| 观测价值 (O) | 10% | 一旦实现正确，是否能显著帮助其他测试定位问题 |

**总分 = 0.35U + 0.35I + 0.2B + 0.1O**

---

## 二、测试组排序

> 原始文档 30 组的评分保持不变，新增组以 **加粗** 标注。共 41 组。
>
>
> 标注说明：
> - **实现**：✅ 完整 ｜ ⚠️ stub/no-op
> - **测试**：🟢 完整测试 ｜ 🟡 特定行为测试 ｜ 🔴 无测试

| 排名 | 测试组 | 代表 syscall | U | I | B | O | 总分 | 实现 | 测试 |
|------|--------|-------------|---|---|---|---|------|------|------|
| 1 | 基本 IO | read/write/lseek/close | 5 | 5 | 5 | 5 | **5.00** | ✅ | 🟡 bug-pipe-fd-errno |
| 2 | open 路径入口 | open/openat | 5 | 5 | 5 | 4 | **4.90** | ✅ | 🟡 bug-open-dir-wronly |
| 3 | 文件元数据 | stat/lstat/fstat/fstatat/statx/statfs/fstatfs | 5 | 4 | 5 | 5 | **4.65** | ✅ | 🔴 |
| 4 | 核心内存映射 | mmap/munmap/mprotect | 5 | 5 | 4 | 4 | **4.65** | ✅ | 🔴 |
| 5 | 进程生命周期 | fork/clone/clone3/execve/wait4/exit/exit_group/getpid/getppid/gettid/set_tid_address | 4 | 5 | 4 | 4 | **4.30** | ✅ | 🟡 clone3-badsize |
| 6 | 核心信号 | rt_sigaction/rt_sigprocmask/kill/tgkill/tkill/rt_sigpending | 4 | 5 | 3 | 4 | **4.10** | ✅ | 🟡 test-sa-restart |
| 7 | 时间与睡眠 | clock_gettime/gettimeofday/nanosleep/clock_nanosleep | 5 | 4 | 3 | 3 | **4.00** | ✅ | 🟢 test-clock-gettime |
| 8 | fd 管理 | dup/dup2/dup3/fcntl/flock/ioctl/close_range | 4 | 4 | 4 | 4 | **4.00** | ✅ | 🟡 test_ioctl_fionbio_int |
| 9 | 管道 | pipe/pipe2 | 4 | 4 | 4 | 4 | **4.00** | ✅ | 🔴 |
| 10 | 路径与目录 | mkdirat/getcwd/chdir/fchdir/chroot/unlinkat/rename/linkat/symlinkat/readlinkat/mknodat/renameat2/getdents64 | 4 | 4 | 3 | 4 | **3.85** | ✅ | 🟡 bug-unlinkat-einval |
| 11 | 网络连接核心 | socket/bind/listen/accept/accept4/connect | 4 | 4 | 3 | 4 | **3.85** | ✅ | 🔴 |
| 12 | 堆扩展 | brk/sbrk | 4 | 4 | 3 | 3 | **3.75** | ✅ | 🔴 |
| 13 | 用户态同步 | futex/get_robust_list/set_robust_list | 3 | 5 | 3 | 3 | **3.60** | ✅ | 🔴 |
| 14 | **进程控制与资源** | **prctl/capget/capset/getrusage/seccomp/arch_prctl/riscv_flush_icache** | 4 | 4 | 3 | 2 | **3.55** | ✅ | 🟢 test-prctl-pdeathsig |
| 15 | 事件复用 | epoll_create1/epoll_ctl/epoll_pwait/epoll_pwait2 | 4 | 4 | 2 | 3 | **3.40** | ✅ | 🟡 test-epoll-pwait-sigsetsize |
| 16 | **凭证与身份** | **getuid/geteuid/getgid/getegid/getresuid/getresgid/setuid/setgid/setreuid/setregid/setresuid/setresgid/getgroups/setgroups** | 4 | 4 | 2 | 3 | **3.40** | ✅ | 🟢 test-credentials, test-getgroups |
| 17 | 传统复用 | select/pselect6/poll/ppoll | 3 | 4 | 3 | 3 | **3.35** | ✅ | 🔴 |
| 18 | 文件权限 | chmod/fchmod/fchmodat/chown/fchown/fchownat/faccessat/umask | 3 | 4 | 2 | 4 | **3.35** | ✅ | 🔴 |
| 19 | socket 数据面 | sendto/recvfrom/sendmsg/recvmsg | 4 | 3 | 2 | 3 | **3.15** | ✅ | 🔴 |
| 20 | 随机数 | getrandom | 3 | 4 | 2 | 3 | **3.15** | ✅ | 🔴 |
| 21 | **文件同步** | **fsync/fdatasync/sync_file_range/sync/syncfs** | 3 | 4 | 2 | 3 | **3.15** | ✅ | 🟡 test-fsync-dir |
| 22 | **调度与亲和性** | **sched_setaffinity/sched_getaffinity/sched_yield/sched_getscheduler/sched_setscheduler/sched_getparam/getpriority** | 3 | 4 | 2 | 2 | **3.05** | ✅ | 🟡 bug-proc-status-affinity |
| 23 | socket 辅助语义 | getsockname/getpeername/getsockopt/setsockopt/shutdown | 3 | 3 | 2 | 3 | **2.85** | ✅ | 🔴 |
| 24 | 向量 IO | readv/writev/pread64/pwrite64/preadv/pwritev/preadv2/pwritev2 | 3 | 3 | 2 | 2 | **2.75** | ✅ | 🟡 bug-pipe-fd-errno |
| 25 | **信号扩展** | **sigaltstack/rt_sigtimedwait/rt_sigsuspend/rt_sigqueueinfo/rt_tgsigqueueinfo/rt_sigreturn** | 2 | 3 | 1 | 2 | **2.30** | ✅ | 🔴 |
| 26 | 系统信息 | uname/sysinfo/syslog | 3 | 2 | 1 | 2 | **2.25** | ✅ | 🔴 |
| 27 | 高级零拷贝 IO | sendfile/copy_file_range/splice | 2 | 3 | 1 | 2 | **2.15** | ✅ | 🔴 |
| 28 | 共享内存 | shmget/shmat/shmctl/shmdt | 2 | 3 | 1 | 2 | **2.15** | ✅ | 🔴 |
| 29 | 定时器 | getitimer/setitimer/timer_create/timer_gettime/timer_settime | 2 | 3 | 2 | 2 | **2.45** | ✅/⚠️ | 🔴 |
| 30 | mmap 扩展 | mremap/mincore/madvise/msync/mlock/mlock2/get_mempolicy | 2 | 3 | 2 | 2 | **2.45** | ✅/⚠️ | 🔴 |
| 31 | socketpair | socketpair | 2 | 3 | 2 | 2 | **2.45** | ✅ | 🔴 |
| 32 | **进程时间统计** | **times/clock_getres** | 2 | 2 | 1 | 2 | **2.00** | ✅ | 🟢 times |
| 33 | 会话与进程组 | setsid/getsid/setpgid/getpgid | 2 | 2 | 2 | 2 | **2.00** | ✅ | 🔴 |
| 34 | **文件空间管理** | **fallocate/fadvise64/truncate/ftruncate** | 2 | 2 | 1 | 2 | **2.00** | ✅ | 🔴 |
| 35 | **资源限制** | **getrlimit/setrlimit（prlimit64）** | 2 | 2 | 1 | 2 | **2.00** | ✅ | 🟡 test-rlimit-stack |
| 36 | 消息队列 | msgget/msgsnd/msgrcv/msgctl | 1 | 2 | 1 | 2 | **1.45** | ✅ | 🔴 |
| 37 | 现代 fd | memfd_create/pidfd_open/pidfd_getfd/pidfd_send_signal | 1 | 2 | 1 | 2 | **1.45** | ✅ | 🔴 |
| 38 | 特殊 fd | eventfd2/signalfd4 | 1 | 2 | 1 | 2 | **1.45** | ✅ | 🔴 |
| 39 | **文件系统挂载** | **mount/umount2** | 1 | 2 | 1 | 1 | **1.35** | ✅ | 🔴 |
| 40 | **文件时间戳** | **utimensat** | 1 | 2 | 1 | 1 | **1.35** | ✅ | 🔴 |
| 41 | **同步扩展** | **membarrier/rseq** | 1 | 2 | 1 | 1 | **1.35** | ✅ | 🔴 |

---

## 三、建议测试实施顺序

### Tier 0: 测试基础设施层

> 先把"可观测、可复现、可诊断"建立起来

1. 基本 IO
2. open/openat
3. stat/fstatat
4. 时间与睡眠
5. pipe/pipe2

说明：

- `stat` 必须前移，因为很多测试都把它当独立观测手段。
- `time` 必须前移，因为很多进程/信号/网络测试都依赖超时、退避、等待。
- `pipe2` 早于 socket，因为它更简单，更适合先验证阻塞、非阻塞、EOF、EPIPE。

### Tier 1: 进程、内存与 fd 核心层

> 有了基础设施后，优先验证最关键的运行时语义

1. fork/clone/clone3/execve/wait4/exit/exit_group/getpid/getppid/gettid/set_tid_address
2. dup/dup2/dup3/fcntl/flock/ioctl/close_range
3. mmap/munmap/mprotect
4. rt_sigaction/rt_sigprocmask/kill/tgkill/tkill
5. brk/sbrk
6. futex/get_robust_list/set_robust_list
7. **进程控制与资源（prctl/prlimit64/seccomp）**
8. **凭证与身份（getuid/setuid/setreuid/setresuid/getresuid/getgroups/setgroups）**

说明：

- `fork` 和 `mmap` 是 Linux 运行时两大核心。
- `signal` 在异常处理和线程信号投递上影响极大，应尽早验证。
- `futex` 在重要性上很高，但实现和定位成本更高，适合放在 Tier 1 末尾。
- **`prctl` 在 PostgreSQL（PDEATHSIG）、容器（PR_SET_NAME）中广泛使用，原始文档未列出。**
- **凭证写操作（setresuid/setgroups 等）是权限模型基础，原始文档仅列了读操作。**
- **ioctl 是 Linux 最广泛的 fd 控制接口，原始文档完全遗漏。**

### Tier 2: 路径、权限与网络主路径

> 这层决定"应用是否能像 Linux 一样真正工作"

1. 目录与路径操作（含 fchdir/chroot/readlinkat/mknodat/renameat2）
2. 文件权限与 `umask`（含 fchownat/fchmodat）
3. **文件同步（fsync/fdatasync/sync_file_range）**
4. **调度与亲和性（sched_setaffinity/sched_getaffinity/sched_getparam/getpriority）**
5. socket/connect/accept/accept4
6. select/poll
7. epoll

说明：

- `select/poll` 先于 `epoll`，因为它更适合用来确认 readiness 的基础语义。
- **`fsync` 族对数据完整性至关重要，PostgreSQL 的 WAL 写入依赖正确行为。**
- **`sched_affinity` 在多核场景和容器调度中广泛使用。**

### Tier 3: 增强语义与高阶能力

1. socket 数据面
2. socket 辅助语义
3. socketpair
4. 向量 IO（含 preadv2/pwritev2）
5. **信号扩展（sigaltstack/rt_sigtimedwait）**
6. 定时器（含 timer_gettime）
7. mmap 扩展（含 mlock2/get_mempolicy）
8. 会话与进程组
9. **进程时间统计（times）**
10. 随机数
11. **文件空间管理（fallocate/fadvise64/truncate/ftruncate）**
12. **资源限制（getrlimit/setrlimit）**

### Tier 4: 低频或专用扩展

1. 系统信息与身份（含 syslog）
2. **文件元数据扩展（statfs/fstatfs）**
3. 高级零拷贝 IO
4. 共享内存
5. 消息队列
6. 现代 fd（含 pidfd_send_signal）
7. 特殊 fd
8. **文件系统挂载（mount/umount2）**
9. **文件时间戳（utimensat）**
10. **同步扩展（membarrier/rseq）**

---

## 四、当前测试覆盖对照

### 已有完整测试

| 测试用例 | 覆盖的测试组 |
|----------|-------------|
| test-clock-gettime | 时间与睡眠 |
| test-credentials | **凭证与身份**（新增组） |
| test-getgroups | **凭证与身份**（新增组） |
| test-prctl-pdeathsig | **进程控制与资源**（新增组） |
| times | **进程时间统计**（新增组） |

### 已有特定行为测试

| 测试用例 | 覆盖的测试组 | 测试的具体行为 |
|----------|-------------|---------------|
| bug-pipe-fd-errno | 基本 IO + 向量 IO | 管道 fd 上 errno 正确性 |
| bug-open-dir-wronly | open 路径入口 | 目录 O_WRONLY → EISDIR |
| clone3-badsize | 进程生命周期 | clone3 超大 size 不 panic |
| test-sa-restart | 核心信号 | SA_RESTART 重启语义 |
| test_ioctl_fionbio_int | fd 管理 | ioctl FIONBIO 读完整 int |
| bug-unlinkat-einval | 路径与目录 | unlinkat 非法 flags → EINVAL |
| test-epoll-pwait-sigsetsize | 事件复用 | sigsetsize 兼容 musl/glibc |
| test-fsync-dir | **文件同步**（新增组） | 目录 fd 上 fsync 成功 |
| bug-proc-status-affinity | **调度与亲和性**（新增组） | /proc/self/status 反映亲和性 |
| test-rlimit-stack | **资源限制**（新增组） | RLIMIT_STACK 默认值 8MB |

### 无测试的高优先级组

| 测试组 | 总分 | 理由 |
|--------|------|------|
| 文件元数据 (stat) | 4.65 | 多处测试用作观测工具，本身无独立测试 |
| 核心内存映射 (mmap) | 4.65 | 动态链接/装载/匿名内存均依赖 |
| 管道 (pipe/pipe2) | 4.00 | 多进程同步、非阻塞语义的基础设施 |
| 堆扩展 (brk) | 3.75 | malloc/free 老路径依赖 |
| 用户态同步 (futex) | 3.60 | pthread 锁核心，重要性高 |
| 传统复用 (select/poll) | 3.35 | epoll 的前置验证 |
| 文件权限 | 3.35 | 安全边界相关 |

---

## 五、已修复 bug 汇总

| 测试用例 | syscall | bug 描述 |
|----------|---------|----------|
| bug-open-dir-wronly | open | 目录 O_WRONLY 返回有效 fd 而非 EISDIR |
| bug-pipe-fd-errno | lseek/pread/pwrite 等 9 个 | File::from_fd() 统一返回 EPIPE 而非各 syscall 正确 errno |
| bug-unlinkat-einval | unlinkat | 不校验 flags，非法比特被静默吞掉 |
| clone3-badsize | clone3 | 超大 size 导致内核 buffer 溢出 panic |
| bug-proc-status-affinity | sched_setaffinity | /proc/self/status Cpus_allowed 未反映实际亲和性 |

---

## 六、原始文档遗漏的 syscall 汇总

| 遗漏的 syscall | 本文档新增组 | 遗漏原因 |
|----------------|-------------|----------|
| prctl / prlimit64 / capget / capset / getrusage / seccomp / arch_prctl / riscv_flush_icache | 进程控制与资源 | 原始文档未单独列出，PostgreSQL 等应用依赖 |
| getuid / geteuid / getgid / getegid / getresuid / getresgid / setuid / setgid / setreuid / setregid / setresuid / setresgid / getgroups / setgroups | 凭证与身份 | 原始"系统信息与身份"组仅列 getuid/getgid，遗漏了写操作和 getresuid/getresgid/setreuid/setregid |
| sched_setaffinity / sched_getaffinity / sched_yield / sched_getscheduler / sched_setscheduler / sched_getparam / getpriority | 调度与亲和性 | 原始文档完全未列出 sched 族和优先级 |
| fsync / fdatasync / sync_file_range / sync / syncfs | 文件同步 | 原始文档未列出 fsync 族 |
| times / clock_getres | 进程时间统计 | 原始"时间与睡眠"组仅列 clock_gettime，未包含 times |
| getrlimit / setrlimit | 资源限制 | 原始文档未列出 |
| sigaltstack / rt_sigtimedwait / rt_sigsuspend / rt_sigqueueinfo | 信号扩展 | 原始"核心信号"组仅列 sigaction/sigprocmask/kill |
| fallocate / fadvise64 / truncate / ftruncate | 文件空间管理 | 原始文档未列出 |
| mount / umount2 | 文件系统挂载 | 原始文档未列出 |
| utimensat | 文件时间戳 | 原始文档未列出 |
| getpid / getppid / gettid / clone3 / exit_group / set_tid_address | 进程生命周期 | 原始文档仅列 fork/clone/execve/wait4/exit，遗漏了 ID 查询、clone3、exit_group |
| tkill | 核心信号 | 原始文档列 kill/tgkill，遗漏了 tkill |
| ioctl / close_range | fd 管理 | 原始文档未列出 ioctl（使用极广）和 close_range |
| fchdir / chroot / mknodat / readlinkat / renameat2 | 路径与目录 | 原始文档仅列 chdir/unlinkat 等基础操作，遗漏了 fchdir/chroot/mknodat/readlinkat/renameat2 |
| accept | 网络连接核心 | 原始文档仅列 accept4 |
| get_robust_list / set_robust_list | 用户态同步 | 原始文档仅列 futex |
| epoll_pwait2 | 事件复用 | 原始文档仅列 epoll_pwait |
| fchownat / fchmodat | 文件权限 | 原始文档仅列 chown/fchown/chmod/fchmod，遗漏了 *at 变体 |
| preadv2 / pwritev2 | 向量 IO | 原始文档仅列 preadv/pwritev |
| syslog | 系统信息 | 原始文档未列出 |
| timer_gettime | 定时器 | 原始文档仅列 timer_create/timer_settime |
| mlock2 / get_mempolicy | mmap 扩展 | 原始文档未列出 mlock2 和 NUMA 策略 |
| statfs / fstatfs | 文件元数据 | 原始文档未列出文件系统级别统计 |
| pidfd_send_signal | 现代 fd | 原始文档仅列 pidfd_open/pidfd_getfd |
| membarrier / rseq | 同步扩展 | 原始文档未列出 |

---

## 七、最终结论

1. 原始 30 组评分保持不变，新增 11 组覆盖 StarryOS 已实现但原始文档遗漏的 syscall。
2. 经与内核分发表（`os/StarryOS/kernel/src/syscall/mod.rs`）逐条核对，本文档现覆盖全部已实现 syscall。
3. StarryOS 内核已实现约 150+ 个 syscall（排除 dummy/stub），当前测试仅覆盖约 25 个（多为特定行为测试）。
4. 最紧迫的测试缺口：**stat**（观测基础）、**pipe**（进程基础）、**mmap**（内存基础）、**futex**（同步基础）、**ioctl**（设备控制基础）。
5. 原始文档遗漏集中在**进程 ID 查询**、**ioctl**、**文件截断**、**文件系统统计**、**调度与亲和性**、**文件同步**、**凭证完整操作**等类别。
