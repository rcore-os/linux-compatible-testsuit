---
marp: true
theme: default
paginate: true
size: 16:9
style: |
  section {
    font-family: "Noto Sans CJK SC", "LXGW WenKai", "Segoe UI", sans-serif;
    font-size: 28px;
    color: #10233f;
    background: linear-gradient(135deg, #f7fafc 0%, #eef6ff 45%, #f5f7fb 100%);
  }
  h1, h2, h3 {
    color: #0b4f6c;
  }
  table {
    font-size: 0.64em;
  }
  code, pre {
    font-family: "IBM Plex Mono", "Fira Code", monospace;
    font-size: 0.9em;
  }
  small {
    color: #4b5d79;
  }
---

# linux-compatible-testsuit
## 如何通过AI自动发现StarryOS的bug并自动修复

**作者**

- 戴骏翔  陈渝

**时间**

- 2026.04.15

---

**分析范围**

- 仓库目录：`linux-compatible-testsuit/`
- 历史窗口：`2026-04-14 16:37` 到 `2026-04-15 02:42`
- 证据来源：`git log`、`README.md`、`run_all_tests.sh`、`docs/session-summary-20260415.md`

| 指标 | 值 |
|---|---:|
| 提交数 | 15 |
| 贡献者 | 2 |
| 时间跨度 | 10 小时 05 分 |
| 历史代码变更量 | +10590 / -1137 |
| 当前测试文件数 | 10 |

<small>结论先行：这是一个在极短时间内完成“从测试草案到三阶段自动化基线”的爆发式开发项目。</small>

---

# 一页结论
0. **设想：如何通过AI自动发现StarryOS的bug，并自动修复。**
1. 项目最初不是从“完整平台”起步，而是从 `README + syscall 优先级 + 一批测试文件 + 单脚本` 快速起盘。
2. 真正的架构跃迁发生在 `2026-04-15 01:48` 的 `91bf3c2`：引入 Linux guest `qemu-system-x86_64` 参考基线，使 StarryOS 不再只和 host / `qemu-user` 间接比较。
3. 随后的开发重点从“多加测试”切换为“修正测试假设”，避免把 musl、root 权限、guest 环境差异误判成内核 bug。
4. 当前仓库已形成三阶段流水线、统一 marker 协议、可比对的结果摘要，但仍存在文档滞后和平台覆盖不足等工程债务。
---


# 一页结论
```text
提交归属

chyyuu    | ########## | 10
wyatt-dai | #####      |  5

占比观察:
- chyyuu    : 66.7%
- wyatt-dai : 33.3%
```

---

# 项目目标

```text
+------------------------+      +---------------------------+
| 目标 OS 是否“像 Linux” | ---> | 不能只靠单个 syscall 手测 |
+------------------------+      +---------------------------+
             |                                |
             v                                v
+------------------------+      +---------------------------+
| 需要一组可重复的测试集 | ---> | 同一批二进制跨环境执行对比 |
+------------------------+      +---------------------------+
             \                                /
              \                              /
               +----------------------------+
               | 输出可解析结果, 快速定位差异 |
               +----------------------------+
```
---

# 项目目标
**从 git 历史反推出的核心目标**

| 目标 | 历史证据 | 含义 |
|---|---|---|
| 建立 Linux 兼容性回归集 | 初始即加入多组 syscall 测试 | 不做单点验证，而做测试套件 |
| 建立分层基线 | `linux-only` -> `linux-system` -> `starry` | 让“不兼容”有更可信的比较对象 |
| 减少环境噪声 | `9ae3584` 多个测试修正 | 将 libc/权限/guest 差异从真实兼容性问题中剥离 |
| 自动化执行与收集日志 | `run_all_tests.sh` 持续扩展 | 把构建、注入、运行、解析串成流水线 |

---

# 开发时间线

```text
2026-04-14
16:37  8087854  first commit
17:07  4f8e319  syscall priority 文档
18:21  055b55e  run_all_tests.sh 初版
18:24  0841a51  test_brk
18:24  0e954dc  test_mmap + test_stat
18:27  099492a  其余主测试文件批量导入
20:28  2be2da5  调整优先级, 完善 README, 扩展测试
21:04  1fcc51b  打通 Linux/Starry x86_64 与日志链路

2026-04-15
00:11  d43b3bf  新增 test_pwritev2
00:29  0aeccdf  删除 test_accept4
01:48  91bf3c2  加入 Linux guest qemu-system 参考基线
02:00  9ae3584  修 5 个测试, 保证 guest 中稳定运行
02:09  cadb7aa  测完自动结束 qemu-system
02:20  9e3e0ee  修复 --starry-only
02:42  dcf3f51  补 session summary
```

| 日期 | 提交数 | 主旋律 |
|---|---:|---|
| 2026-04-14 | 8 | 起盘、导入测试、建立脚本骨架 |
| 2026-04-15 | 7 | 引入 guest 基线、稳定化、补文档 |

---

# 阶段拆分

| 阶段 | 代表提交 | 主要动作 | 阶段产物 |
|---|---|---|---|
| Phase 0: 立项 | `8087854` | 创建项目名与最小 README | 项目占位 |
| Phase 1: 测试与脚本骨架 | `4f8e319` `055b55e` `0841a51` `0e954dc` `099492a` | 引入优先级文档、主脚本、10 组左右测试文件 | 基本测试仓库成形 |
| Phase 2: 重新排序与可执行化 | `2be2da5` `1fcc51b` | 重写优先级、完善 README、打通 Linux/Starry x86_64 执行链路 | 能构建、能跑、能出日志 |
| Phase 3: 基线升级 | `91bf3c2` `cadb7aa` | 新增 Linux guest `qemu-system` 参考模式、rootfs 生成、自动退出 | 三阶段架构成形 |
| Phase 4: 语义稳定化 | `9ae3584` `9e3e0ee` `dcf3f51` | 修测试假设、修选项路径、记录会话总结 | 结果更可信, 可复盘 |

---

# 阶段拆分

```text
项目节奏不是线性“加功能”

  骨架搭建
      |
      v
  批量导入测试
      |
      v
  跑起来
      |
      v
  找到测试噪声
      |
      v
  建 Linux guest 基线
      |
      v
  让失败更像“真实兼容性失败”
```

---

# 架构演进

```text
V0: 仅有测试和想法
  [README] [priority doc] [tests/*.c]

V1: 初版自动化
  tests/*.c -> run_all_tests.sh -> Linux host / qemu-user

V2: StarryOS 接入
  tests/*.c -> 静态编译 -> 注入 ext4 -> QEMU(StarryOS) -> 串口日志

V3: 当前形态
  同一批测试二进制
        |
        +--> Linux host / qemu-user
        +--> Linux guest(qemu-system-x86_64)
        +--> StarryOS
                     |
                     v
            统一 marker + 汇总对比
```

---

# 架构演进

**演进重点**

| 版本 | 设计重心 | 价值 |
|---|---|---|
| V1 | “能跑” | 先把 syscall 测试串起来 |
| V2 | “能进 StarryOS rootfs” | 从 Linux 走向目标 OS |
| V3 | “有可比基线” | 避免把 host / `qemu-user` 当成唯一真相 |

---

# 当前系统设计

```text
                        +----------------------+
                        |      tests/*.c       |
                        +----------+-----------+
                                   |
                                   v
                        +----------------------+
                        |   run_all_tests.sh   |
                        +----+-----------+-----+
                             |           |
              +--------------+           +------------------+
              |                                         |
              v                                         v
  +---------------------------+            +---------------------------+
  | Phase 1                   |            | Phase 1.5 / Phase 2       |
  | Linux host / qemu-user    |            | Linux guest / StarryOS    |
  +-------------+-------------+            +-------------+-------------+
                |                                        |
                v                                        v
  +---------------------------+            +---------------------------+
  | compile / run logs        |            | @@@ STARRY_TEST_* marker  |
  +-------------+-------------+            +-------------+-------------+
                \                                        /
                 \                                      /
                  v                                    v
                    +--------------------------------+
                    | summary + per-test status      |
                    +---------------+----------------+
                                    |
                                    v
                    +--------------------------------+
                    | Linux reference vs StarryOS    |
                    +--------------------------------+
```
---

# 当前系统设计

| 模块 | 作用 | 历史来源 |
|---|---|---|
| `initialize_test_programs()` | 运行时发现测试文件 | `91bf3c2` 后不再硬编码旧测试表 |
| Linux guest rootfs 生成 | 自动做 ext4 镜像并注入 busybox/runner/tests | `91bf3c2` |
| marker 协议 | 统一输出 `@@@ STARRY_TEST_*` | 先服务 StarryOS，后复用到 Linux guest |
| 结果对比 | `compare_starry_with_linux_reference()` | 当前脚本已经支持逐项对比 |

---

# 测试内容与覆盖面

| 层级 | 设计优先级来源 | 当前覆盖 |
|---|---|---|
| Tier 0 基础设施 | `basic_io` `openat` `stat` `pipe2` | 已覆盖 |
| Tier 1 核心运行时 | `fork_v2` `dup_v2` `mmap` `signal` `brk` | 已覆盖 |
| 补充点 | `pwritev2` | 已加入 |
| 仍缺失的高价值组 | `time/sleep` `futex` `socket/connect/accept` `select/poll` `epoll` | 待补 |

---

# 测试内容与覆盖面

| 当前测试文件 | 状态 |
|---|---|
| `test_basic_io.c` | 在仓库中 |
| `test_openat.c` | 在仓库中 |
| `test_stat.c` | 在仓库中 |
| `test_pipe2.c` | 在仓库中 |
| `test_fork_v2.c` | 在仓库中 |
| `test_dup_v2.c` | 在仓库中 |
| `test_mmap.c` | 在仓库中 |
| `test_signal.c` | 在仓库中 |
| `test_brk.c` | 在仓库中 |
| `test_pwritev2.c` | 在仓库中 |

<small>注意：当前 `tests/` 中已经没有 `test_accept4.c`，但 README 仍保留旧列表，这是一个明显的文档漂移信号。</small>

---

# 关键转折：测试从“严格”走向“可信”

`9ae3584` 是项目质量拐点。它没有大量新增功能，而是在修“测试本身的错误假设”。

| 测试 | 修正前问题 | 修正后策略 | 真实含义 |
|---|---|---|---|
| `test_brk.c` | 把某些 `brk/sbrk` 行为写死为必须成功 | 接受 static musl 下 `ENOMEM` 分支 | 过滤 libc/环境差异 |
| `test_dup_v2.c` | 假定 `dup3` 未知 flags 必为 `EINVAL` | 接受“成功或 `EINVAL`” | 避免 wrapper 差异误报 |
| `test_fork_v2.c` | 依赖 guest 里存在 `/bin/true` | 改为 `execve` 自身二进制 | 去掉 rootfs 外部依赖 |
| `test_mmap.c` | 依赖子进程写只读页触发 SIGSEGV | 改为只验证保护切换可读写 | 让 suite 更稳定、少噪声 |
| `test_stat.c` | 直接 `stat(path, NULL)` / 假定非 root 权限模型 | 改用 raw syscall, 区分 root 情况 | 不把 libc 和权限特例算成 kernel 失败 |

---

# 测试内容与覆盖面

```text
修正原则

  不是把测试放宽到“什么都算对”
  而是把测试改成：

    真实 syscall 语义  >  wrapper 假设
    可复现失败         > 花哨但脆弱的断言
    guest 可移植性      > 宿主机偶然成立的条件
```

---

# 里程碑提交

| 提交 | 时间 | 变化规模 | 价值判断 |
|---|---|---:|---|
| `055b55e` | 2026-04-14 18:21 | +700 | 一次性放入主脚本，项目开始“可执行化” |
| `099492a` | 2026-04-14 18:27 | +2962 | 主测试文件批量导入，形成第一批测试库 |
| `2be2da5` | 2026-04-14 20:28 | +617 / -412 | 从“上传文件”转向“有优先级、有说明、有组织” |
| `1fcc51b` | 2026-04-14 21:04 | +412 / -258 | Linux/Starry x86_64 与 rootfs 注入链路成形 |
| `91bf3c2` | 2026-04-15 01:48 | +3334 / -63 | 最大架构升级，引入 Linux guest 参考模式 |
| `9ae3584` | 2026-04-15 02:00 | +100 / -46 | 把“能跑”提升为“结果可信” |

---

# 测试内容与覆盖面

```text
脚本初版
   |
   v
测试批量导入
   |
   v
README / priority / StarryOS 路径
   |
   v
Linux guest baseline
   |
   v
测试稳定化
   |
   v
session summary
```

---

# 进展情况

| 维度 | 当前状态 | 说明 |
|---|---|---|
| 测试集骨架 | `[x]` | 10 个测试文件, 已覆盖 Tier 0/Tier 1 主体 |
| Linux host / qemu-user | `[x]` | 早期即存在 |
| Linux guest 参考基线 | `[x]` | `x86_64` 已打通 |
| StarryOS rootfs 注入与自动运行 | `[x]` | 已通过 `debugfs` 注入, 不依赖 `sudo mount` |
| Linux vs StarryOS 自动对比 | `[x]` | 已有统一 marker 和汇总 |
| 文档闭环 | `[~]` | 有 README、priority、session summary, 但 README 已滞后 |
| 平台广度 | `[~]` | guest 仅 `x86_64`; StarryOS 虽支持多架构参数, 历史证据主要集中在 `x86_64` |
| CI / 趋势追踪 | `[ ]` | git 历史中尚未看到 |

---

# 进展情况

```text
成熟度观察

测试内容        [########..]  以基础 syscall 为主
自动化程度      [#########.]  构建/注入/运行/解析都已有
可比性          [#########.]  已有 Linux guest 参考层
工程闭环        [######....]  仍缺 CI、趋势看板、文档同步
```

---

# 当前问题与风险

| 类型 | 现象 | 风险 |
|---|---|---|
| 文档漂移 | README 仍列 `test_accept4.c`，实际已换成 `test_pwritev2.c` | 汇报、使用、维护时容易误解当前覆盖面 |
| 平台覆盖不足 | Linux guest 明确只有 `x86_64` | 基线可信，但横向推广不足 |
| 依赖本地环境 | 依赖 musl 工具链、`busybox-static`、`tgoskits`、内核镜像 | 新环境复现成本偏高 |
| 缺少持续回归机制 | 历史中未见 CI / 自动报表 | 兼容性进展难做长期趋势追踪 |
| 结果记录偏会话化 | `session-summary-20260415.md` 很有价值，但仍是一次性总结 | 后续结果沉淀可能分散 |

```text
最值得优先处理的不是“继续堆测试文件”
而是：

  1. 先消除文档漂移
  2. 再把基线运行流程固化为可持续回归
  3. 然后扩到 futex / time / socket / poll / epoll
```

---

# 建议的下一阶段路线图 -- 方案1

```text
短期  : 修 README / 同步测试清单
   |
   v
短期  : 固化 Linux guest 基线运行文档
   |
   v
中期  : 增加 time,sleep,futex,socket 基础测试
   |
   v
中期  : 加 CI 或 nightly 回归
   |
   v
长期  : 扩展 guest 到更多架构
   |
   v
长期  : 建兼容性趋势面板
```

---

# 建议的下一阶段路线图 -- 方案2

| 时间层次 | 建议动作 | 原因 |
|---|---|---|
| 短期 | 同步 README 与真实测试集 | 先保证信息正确 |
| 短期 | 把 session summary 抽象成固定报告模板 | 降低一次性总结的偶然性 |
| 中期 | 补齐 `time/sleep`、`futex`、`socket`、`select/poll` | 这些是下一批高价值 syscall |
| 中期 | 为 `run_all_tests.sh` 加持续回归入口 | 让兼容性进度可跟踪 |
| 长期 | guest 多架构化 | 让基线更广泛、更稳健 |

---

# 总结

```text
这不是“先设计很多, 再慢慢实现”的项目
而是一个非常典型的工程推进路径:

  先搭骨架
  再把链路跑通
  再建立更可信的基线
  最后修正测试假设, 提升结果质量
```
---

# 总结


**最终判断**

- `linux-compatible-testsuit` 已经从“测试文件集合”演进成“有三阶段执行架构的兼容性验证框架”。
- 项目最强的设计点不是测试数量，而是 **同一批静态二进制跨 Linux host / Linux guest / StarryOS 复用**。
- 项目当前最需要补的不是更多临时实验，而是 **文档同步、持续回归和更高优先级 syscall 的继续补齐**。

<small>附注：本分析严格基于 `linux-compatible-testsuit/` 目录的 git 历史与当前文件树，不包含仓库外部口头背景。</small>
