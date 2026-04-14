# Linux 系统调用测试优先级与实施顺序

## 目标

本文档面向本仓库的 **Linux 兼容性测试**，按“重要性”和“通用性”对 syscall 测试组重新排序。

这里刻意区分两件事：

1. **价值优先级**：哪些 syscall 对 Linux 用户态最重要、最常见、出错影响最大。
2. **实施顺序**：在编写和 bring-up 测试时，应该先测什么，后测什么。

旧版本的主要问题，是评分表、tier 分层和实际依赖链并不完全一致。尤其是：

- `stat` 在当前仓库里已经是大量测试的独立观测手段，不应继续放得太后。
- `pipe/pipe2` 是多进程和非阻塞语义验证的基础，不应落到网络之后。
- `time/sleep` 在实际测试稳定性里很重要，不能只按“附属功能”看待。
- `epoll` 应排在 `select/poll` 之后做验证，先确认基础 readiness 语义，再确认 epoll 语义。

---

## 一、排序原则

### 1. 重要性优先

如果某类 syscall 出错会直接导致：

- 进程模型失效
- 内存安全问题
- 数据损坏
- 大量应用无法启动

则优先级必须靠前。

### 2. 通用性优先

如果某类 syscall 被 libc、shell、编译器、文件工具、服务进程、常见应用广泛依赖，则优先级必须靠前。

### 3. 依赖链优先

很多测试本身需要依赖其他 syscall 才能完成观测和断言，因此实施顺序不能只看抽象分数，还要看“谁是测试基础设施”。

对本仓库而言，最典型的依赖链是：

`basic io -> openat -> stat -> pipe2 -> fork/wait -> socket -> select/poll -> epoll`

### 4. 先验证基础语义，再验证增强语义

例如：

- 先测 `socket/connect/accept`
- 再测 `sendmsg/recvmsg/getsockopt`
- 先测 `select/poll`
- 再测 `epoll`
- 先测 `mmap/munmap/mprotect`
- 再测 `mremap/mincore/madvise/msync`

---

## 二、评分模型

每个 syscall 测试组按 4 个维度打分：

| 维度 | 权重 | 含义 |
|------|------|------|
| 通用性 (U) | 35% | 典型 Linux 用户态程序使用范围有多广 |
| 重要性 (I) | 35% | 出 bug 后对功能正确性、安全性、稳定性的影响有多大 |
| 基础性 (B) | 20% | 是否是其他测试或其他子系统的依赖前提 |
| 观测价值 (O) | 10% | 一旦实现正确，是否能显著帮助其他测试定位问题 |

**总分 = 0.35U + 0.35I + 0.2B + 0.1O**

评分解释：

- `5` = 极高
- `4` = 高
- `3` = 中
- `2` = 低
- `1` = 很低

说明：

- 这里不再把“复杂度”直接并入优先级总分。
- 复杂度会影响测试实施节奏，但不应该压低高价值 syscall 的排序。

---

## 三、按重要性与通用性重排后的测试组

下面的排序是 **价值优先级**，不是编码顺序。

| 排名 | 测试组 | 代表 syscall | U | I | B | O | 总分 | 说明 |
|------|--------|-------------|---|---|---|---|------|------|
| 1 | 基本 IO | read/write/lseek/close | 5 | 5 | 5 | 5 | **5.00** | 最基础的数据通路，几乎一切用户态都依赖 |
| 2 | open 路径入口 | open/openat | 5 | 5 | 5 | 4 | **4.90** | 文件、目录、执行文件、设备节点访问入口 |
| 3 | 文件元数据 | stat/lstat/fstat/fstatat/statx | 5 | 4 | 5 | 5 | **4.65** | 路径解析、权限判断、独立观测都依赖它 |
| 4 | 核心内存映射 | mmap/munmap/mprotect | 5 | 5 | 4 | 4 | **4.65** | 动态链接、装载、匿名内存、保护语义核心 |
| 5 | 进程生命周期 | fork/clone/execve/wait4/exit | 4 | 5 | 4 | 4 | **4.30** | shell、测试框架、服务模型、并发基础 |
| 6 | 核心信号 | rt_sigaction/rt_sigprocmask/kill/tgkill/rt_sigpending | 4 | 5 | 3 | 4 | **4.10** | 异常处理、终止控制、线程级信号交付核心 |
| 7 | 时间与睡眠 | clock_gettime/gettimeofday/nanosleep/clock_nanosleep | 5 | 4 | 3 | 3 | **4.00** | 超时、重试、调度退避、测试稳定性常用依赖 |
| 8 | fd 管理 | dup/dup2/dup3/fcntl/flock | 4 | 4 | 4 | 4 | **4.00** | 重定向、CLOEXEC、非阻塞、锁语义基础 |
| 9 | 管道 | pipe/pipe2 | 4 | 4 | 4 | 4 | **4.00** | 最简 IPC、shell pipeline、多进程同步基础 |
| 10 | 路径与目录 | mkdirat/getcwd/chdir/unlinkat/rename/linkat/symlinkat/getdents64 | 4 | 4 | 3 | 4 | **3.85** | 文件树操作和路径管理核心 |
| 11 | 网络连接核心 | socket/bind/listen/accept4/connect | 4 | 4 | 3 | 4 | **3.85** | 本地/网络通信基础，服务型应用核心路径 |
| 12 | 堆扩展 | brk/sbrk | 4 | 4 | 3 | 3 | **3.75** | 老路径 `malloc`/`free` 仍依赖，虽不如 mmap 普适但很关键 |
| 13 | 用户态同步 | futex | 3 | 5 | 3 | 3 | **3.60** | pthread 锁/条件变量核心，线程程序正确性关键 |
| 14 | 事件复用 | epoll_create1/epoll_ctl/epoll_pwait | 4 | 4 | 2 | 3 | **3.40** | 现代 Linux 服务器常用，但建立在更基础 IO 语义之上 |
| 15 | 传统复用 | select/pselect6/poll/ppoll | 3 | 4 | 3 | 3 | **3.35** | 兼容性广，适合作为 readiness 基础语义参照 |
| 16 | 文件权限 | chmod/fchmod/chown/fchown/faccessat/umask | 3 | 4 | 2 | 4 | **3.35** | 安全边界、创建语义、权限检查的重要组成部分 |
| 17 | socket 数据面 | sendto/recvfrom/sendmsg/recvmsg | 4 | 3 | 2 | 3 | **3.15** | 连接建立后真正的数据收发语义 |
| 18 | 随机数 | getrandom | 3 | 4 | 2 | 3 | **3.15** | 安全敏感，但依赖面不如文件/进程/内存基础能力广 |
| 19 | socket 辅助语义 | getsockname/getpeername/getsockopt/setsockopt/shutdown | 3 | 3 | 2 | 3 | **2.85** | 连接调优和状态确认，通常晚于连接核心 |
| 20 | 向量 IO | readv/writev/pread64/pwrite64/preadv/pwritev | 3 | 3 | 2 | 2 | **2.75** | 基本 IO 的扩展语义，重要但不是 bring-up 首要项 |
| 21 | 定时器 | getitimer/setitimer/timer_create/timer_settime | 2 | 3 | 2 | 2 | **2.45** | 应用场景明确，但普适性低于时间基础调用 |
| 22 | mmap 扩展 | mremap/mincore/madvise/msync/mlock | 2 | 3 | 2 | 2 | **2.45** | 建立在核心内存映射正确之上 |
| 23 | socketpair | socketpair | 2 | 3 | 2 | 2 | **2.45** | 常见于本机 IPC，但覆盖面低于 pipe/socket 主路径 |
| 24 | 会话与进程组 | setsid/getsid/setpgid/getpgid | 2 | 2 | 2 | 2 | **2.00** | 守护进程和 shell 作业控制需要，但非第一层基础 |
| 25 | 系统信息与身份 | uname/sysinfo/getuid/getgid/geteuid/getegid | 3 | 2 | 1 | 2 | **2.25** | 常用但风险低，容易单独验证 |
| 26 | 高级零拷贝 IO | sendfile/copy_file_range/splice | 2 | 3 | 1 | 2 | **2.15** | 性能和优化向功能，不是兼容性最前线 |
| 27 | 共享内存 | shmget/shmat/shmctl/shmdt | 2 | 3 | 1 | 2 | **2.15** | 较老的 SysV IPC，覆盖面有限 |
| 28 | 消息队列 | msgget/msgsnd/msgrcv/msgctl | 1 | 2 | 1 | 2 | **1.45** | 使用面窄，通常不会阻塞现代用户态 bring-up |
| 29 | 现代 fd | memfd_create/pidfd_open/pidfd_getfd | 1 | 2 | 1 | 2 | **1.45** | 新特性，价值高但通用性不高 |
| 30 | 特殊 fd | eventfd2/signalfd4 | 1 | 2 | 1 | 2 | **1.45** | 主要服务特定事件模型，不是基础兼容性的第一优先级 |

---

## 四、建议测试实施顺序

这一节是 **编码和 bring-up 顺序**，和上一节的“价值排序”不同。

### Tier 0: 测试基础设施层

> 先把“可观测、可复现、可诊断”建立起来

1. 基本 IO
2. open/openat
3. stat/fstatat
4. 时间与睡眠
5. pipe/pipe2

说明：

- `stat` 必须前移，因为当前仓库很多测试都把它当独立观测手段。
- `time` 必须前移，因为很多进程/信号/网络测试都依赖超时、退避、等待。
- `pipe2` 早于 socket，因为它更简单，更适合先验证阻塞、非阻塞、EOF、EPIPE。

### Tier 1: 进程、内存与 fd 核心层

> 有了基础设施后，优先验证最关键的运行时语义

1. fork/clone/execve/wait4
2. dup/dup2/dup3/fcntl/flock
3. mmap/munmap/mprotect
4. rt_sigaction/rt_sigprocmask/kill
5. brk/sbrk
6. futex

说明：

- `fork` 和 `mmap` 是 Linux 运行时两大核心。
- `signal` 在异常处理和线程信号投递上影响极大，应尽早验证。
- `futex` 在重要性上很高，但实现和定位成本更高，适合放在 Tier 1 末尾。

### Tier 2: 路径、权限与网络主路径

> 这层决定“应用是否能像 Linux 一样真正工作”

1. 目录与路径操作
2. 文件权限与 `umask`
3. socket/connect/accept
4. select/poll
5. epoll

说明：

- `select/poll` 先于 `epoll`，因为它更适合用来确认 readiness 的基础语义。
- `epoll` 虽然在服务器场景里很重要，但不是第一批 bring-up 的起点。

### Tier 3: 增强语义与高阶能力

1. socket 数据面
2. socket 辅助语义
3. socketpair
4. 向量 IO
5. 定时器
6. mmap 扩展
7. 会话与进程组
8. 随机数

说明：

- `getrandom` 的安全意义较高，但对当前兼容性测试基础设施不构成阻塞。
- 向量 IO、socket 辅助语义适合在主路径正确后补全。

### Tier 4: 低频或专用扩展

1. 系统信息与身份
2. 高级零拷贝 IO
3. 共享内存
4. 消息队列
5. 现代 fd
6. 特殊 fd

说明：

- 这类能力要么依赖面窄，要么更偏特定框架和特定工作负载。
- 可以在核心兼容性稳定之后逐步补齐。

---

## 五、当前仓库对应关系

下面按“建议实施顺序”映射到当前仓库。

| 顺序层级 | 测试组 | 当前文件 | 状态 |
|----------|--------|----------|------|
| Tier 0 | 基本 IO | `tests/test_basic_io.c` | 已存在 |
| Tier 0 | open/openat | `tests/test_openat.c` | 已存在 |
| Tier 0 | stat/fstatat | `tests/test_stat.c` | 已存在 |
| Tier 0 | 时间与睡眠 | 无 | 待补充 |
| Tier 0 | pipe/pipe2 | `tests/test_pipe2.c` | 已存在 |
| Tier 1 | fork/clone/execve/wait4 | `tests/test_fork_v2.c` | 已存在 |
| Tier 1 | dup/fcntl/flock | `tests/test_dup_v2.c` | 已存在 |
| Tier 1 | mmap/munmap/mprotect | `tests/test_mmap.c` | 已存在 |
| Tier 1 | 核心信号 | `tests/test_signal.c` | 已存在 |
| Tier 1 | brk/sbrk | `tests/test_brk.c` | 已存在 |
| Tier 1 | futex | 无 | 待补充 |
| Tier 2 | 目录与路径操作 | 无 | 待补充 |
| Tier 2 | 文件权限与 umask | 无 | 待补充 |
| Tier 2 | socket/connect/accept | `tests/test_accept4.c` | 已存在 |
| Tier 2 | select/poll | 无 | 待补充 |
| Tier 2 | epoll | 无 | 待补充 |
| Tier 3 | socket 数据面 | 无 | 待补充 |
| Tier 3 | socket 辅助语义 | 无 | 待补充 |
| Tier 3 | socketpair | 无 | 待补充 |
| Tier 3 | 向量 IO | 无 | 待补充 |
| Tier 3 | 定时器 | 无 | 待补充 |
| Tier 3 | mmap 扩展 | 无 | 待补充 |
| Tier 3 | 会话与进程组 | 可拆自 `tests/test_fork_v2.c` | 部分覆盖 |
| Tier 3 | 随机数 | 无 | 待补充 |
| Tier 4 | 系统信息与身份 | 无 | 待补充 |
| Tier 4 | 高级零拷贝 IO | 无 | 待补充 |
| Tier 4 | 共享内存 | 无 | 待补充 |
| Tier 4 | 消息队列 | 无 | 待补充 |
| Tier 4 | 现代 fd | 无 | 待补充 |
| Tier 4 | 特殊 fd | 无 | 待补充 |

---

## 六、建议的下一轮补测顺序

如果以当前仓库为起点，建议下一轮新增测试按下面顺序推进：

1. `test_time.c`
2. `test_futex.c`
3. `test_dir.c`
4. `test_fileperm.c`
5. `test_poll.c`
6. `test_epoll.c`
7. `test_socket_io.c`
8. `test_sockopt.c`
9. `test_iov.c`
10. `test_getrandom.c`

理由：

- 这一组能直接把当前仓库从“文件/进程/内存主路径”扩到“同步、路径、事件、网络数据面”。
- 同时它们与现有测试风格最容易对齐，不需要先引入特权操作或复杂环境。

---

## 七、最终结论

重排后的核心判断是：

1. `basic io`、`open/openat`、`stat`、`mmap`、`fork/exec/wait` 是 Linux 兼容性测试的第一优先级。
2. `pipe2` 和 `time` 必须前移，因为它们是实际测试基础设施的一部分，而不只是“普通功能组”。
3. `epoll` 很重要，但测试实施顺序上应该晚于 `select/poll` 和 socket 主路径。
4. `futex` 在重要性上属于高优先级，但在测试编排上应放在核心进程/内存语义稳定之后。
5. `getrandom` 不应继续埋在“系统信息”里，它比 `uname/sysinfo/getuid` 更值得单独测试。

这份排序更适合本仓库当前目标：

- 先建立 Linux 兼容性的最小闭环
- 再扩展到并发、网络、事件驱动
- 最后补齐低频和专用特性
