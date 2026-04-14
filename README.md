# linux-compatible-testsuit

一组面向 Linux 兼容性的 syscall 测试，用于验证内核或运行时在文件、进程、内存、信号、管道和网络等基础能力上的语义是否接近 Linux。

当前仓库支持两种运行方式：

- **Linux 多架构模式**：在宿主 Linux 上直接运行，或通过 QEMU user-mode 运行交叉架构静态二进制
- **StarryOS 模式**：将测试程序注入 rootfs，在 QEMU 中启动 StarryOS 后自动执行

## 当前已包含的测试

按当前建议实施顺序，仓库已有这些测试：

1. `tests/test_basic_io.c`
2. `tests/test_openat.c`
3. `tests/test_stat.c`
4. `tests/test_pipe2.c`
5. `tests/test_fork_v2.c`
6. `tests/test_dup_v2.c`
7. `tests/test_mmap.c`
8. `tests/test_signal.c`
9. `tests/test_brk.c`
10. `tests/test_accept4.c`

测试框架头文件位于 `tests/test_framework.h`。大部分较新的测试采用统一的 `CHECK/CHECK_RET/CHECK_ERR` 输出格式，便于定位失败的 syscall、文件和行号。

## 运行

### 只跑 Linux 本地与多架构测试

```bash
./run_all_tests.sh --linux-only
```

### 只跑 StarryOS

```bash
./run_all_tests.sh --starry-only --tgoskits ../tgoskits
```

### 跳过依赖检查

```bash
./run_all_tests.sh --linux-only --no-dep-check
```

## 主要参数

- `--linux-only`: 只运行 Linux 阶段
- `--starry-only`: 只运行 StarryOS 阶段
- `--starry-arch ARCH`: 指定 StarryOS 目标架构，默认 `riscv64`
- `--tgoskits DIR`: 指定 `tgoskits` 仓库路径
- `--no-dep-check`: 跳过依赖检查

环境变量：

- `TGOSKITS_DIR`
- `STARRY_ARCH`
- `STARRY_TIMEOUT`

## 依赖

Ubuntu/Debian 下可参考：

```bash
sudo apt install gcc-riscv64-linux-gnu gcc-aarch64-linux-gnu \
    qemu-user-static qemu-system-misc qemu-system-arm \
    rustc cargo sudo e2fsprogs
```

宿主原生 `x86_64` 运行只要求本机可用 `gcc`。跨架构 Linux 测试还需要对应交叉编译器和 QEMU user-mode。

## 输出

- 构建产物默认位于 `build/`
- 日志默认位于 `logs/`

## 文档

- syscall 重要性和实施顺序见 `docs/syscall_priority.md`

## 当前已知情况

- `run_all_tests.sh` 当前只纳入了仓库里已经存在的测试文件，不再引用缺失的 `futex` 用例。
- `test_accept4.c` 依赖 socket 能力；如果运行环境限制网络相关系统调用，测试可能因为环境限制而失败，而不是实现本身错误。
