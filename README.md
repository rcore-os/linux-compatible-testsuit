# linux-compatible-testsuit
Syscall Test Programs
Linux 系统调用测试程序集，用于在 Linux 多架构环境及 StarryOS 上验证系统调用的正确性。

仓库结构  
├── test_program/ # 测试源码目录（脚本自动扫描此目录下所有 .c 文件）  
├── run_all_tests.sh # 主测试脚本 └── README.md  

新增测试只需在 test_program/ 下放入 .c 文件，脚本会自动发现并编译运行。若源码中包含 pthread 相关调用，脚本会自动添加 -lpthread 并延长超时。

前置依赖
musl 交叉编译工具链,详见starryos仓库readme

快速开始
确保本仓库与 tgoskits（StarryOS）处于同一父目录：

workspace/ ├── tgoskits/ # StarryOS 仓库 └── test/ # 本仓库

然后执行：

bash ./run_all_tests.sh

脚本会自动完成两个阶段： 1. Phase 1 — 在 Linux 上交叉编译并以多架构运行（x86_64 本地、riscv64 via QEMU user） 2. Phase 2 — 将测试程序注入 StarryOS rootfs，启动 QEMU 运行

用法
```bash

完整运行（Linux 多架构 + StarryOS）
./run_all_tests.sh

仅运行 Linux 多架构测试
./run_all_tests.sh --linux-only

仅运行 StarryOS 测试，指定架构
./run_all_tests.sh --starry-only --starry-arch riscv64

手动指定 tgoskits 路径
./run_all_tests.sh --tgoskits /path/to/tgoskits

跳过依赖检查
./run_all_tests.sh --no-dep-check ```

环境变量
变量	说明	默认值
TGOSKITS_DIR	tgoskits 仓库路径	自动检测 ../tgoskits
STARRY_ARCH	StarryOS 目标架构	riscv64
STARRY_TIMEOUT	QEMU 总超时（秒）	60
MUSL_CROSS_ROOT	musl 工具链根目录	/home/wyatt/package
命令行选项
选项	说明
--linux-only	只运行 Phase 1（Linux 多架构）
--starry-only	只运行 Phase 2（StarryOS）
--starry-arch ARCH	指定 StarryOS 目标架构
--tgoskits DIR	指定 tgoskits 路径
--no-dep-check	跳过依赖检查
-h, --help	显示帮助
测试流程
Phase 1: Linux 多架构
自动扫描 test_program/*.c
使用 musl 静态编译（-static）为目标架构生成二进制
本地架构直接运行，其他架构通过 qemu-user-static 运行
每个测试独立超时，pthread 测试自动延长至 120s
Phase 2: StarryOS
用 musl 交叉编译器生成静态二进制（riscv64 等）
下载 StarryOS rootfs（cargo starry rootfs，自动缓存）
制作 rootfs 工作副本并扩容
挂载 rootfs，注入测试二进制和运行脚本
编译 StarryOS 内核
启动 QEMU，init shell 自动执行测试并退出
从串口输出中解析测试结果
恢复原始 rootfs
输出
build/ — 编译产物（按架构分目录）
logs/ — 运行日志
终端实时显示测试进度和结果
最终输出汇总报告
新增测试
在 test_program/ 下添加 .c 文件即可，无需修改脚本。脚本会：

按 *.c 文件名自动生成二进制名（如 test_brk.c → test_brk）
检测源码中是否使用 pthread，自动添加 -lpthread
pthread 测试自动获得更长超时（120s）
