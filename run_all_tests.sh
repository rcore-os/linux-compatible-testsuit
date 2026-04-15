#!/usr/bin/env bash
#
# run_all_tests.sh - Compile and run C syscall test programs
#
# Phases:
#   Phase 1   - Linux user-mode (bare / QEMU user-static) [opt-in via --bare]
#   Phase 1.5 - Linux QEMU system-mode with custom rootfs   [default]
#   Phase 2   - StarryOS tests                               [default]
#
# Usage:
#   ./run_all_tests.sh [OPTIONS]
#
# Options:
#   --bare, --user-mode   Also run Phase 1 (Linux user-mode bare tests)
#   --linux-only          Only run Phase 1.5 (Linux QEMU system-mode tests)
#   --starry-only         Only run Phase 2 (StarryOS tests)
#   --linux-arch ARCH     Target architecture for Phase 1.5 (default: host arch)
#   --starry-arch ARCH    Target architecture for StarryOS (default: riscv64)
#   --tgoskits DIR        Path to tgoskits repo (default: auto-detect ../tgoskits)
#   --no-dep-check        Skip dependency checking
#   -h, --help            Show this help message
#
# Environment variables:
#   TGOSKITS_DIR       Path to tgoskits repo
#   STARRY_ARCH        Default StarryOS target arch
#   STARRY_TIMEOUT     Per-test QEMU timeout in seconds (default: 60)
#   LINUX_KERNEL_VERSION  Linux kernel version to download (default: 6.12.7)
#
# Default: runs Phase 1.5 + Phase 2
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install qemu-system-misc qemu-user-static e2fsprogs \
#                    flex bison bc libelf-dev rustc cargo
#

set -euo pipefail

# ============================================================
#  Configuration
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TGOSKITS_DIR="${TGOSKITS_DIR:-}"
STARRY_ARCH="${STARRY_ARCH:-riscv64}"
STARRY_TIMEOUT="${STARRY_TIMEOUT:-60}"
HOST_ARCH="$(uname -m)"
[[ "$HOST_ARCH" == "amd64" ]] && HOST_ARCH="x86_64"

# Colors
C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[1;33m'
C_BLUE='\033[0;34m'; C_CYAN='\033[0;36m'; C_BOLD='\033[1m'; C_RESET='\033[0m'

# Logging — output to stderr so stdout captures ($()) stay clean
info()    { echo -e "  ${C_BLUE}INFO${C_RESET}  $*" >&2; }
ok()      { echo -e "  ${C_GREEN} OK ${C_RESET}  $*" >&2; }
warn()    { echo -e "  ${C_YELLOW}WARN${C_RESET}  $*" >&2; }
err()     { echo -e "  ${C_RED} ERR${C_RESET}  $*" >&2; }
section() { echo -e "\n${C_BOLD}${C_CYAN}──── $* ────${C_RESET}\n" >&2; }
header()  { echo -e "\n${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}" >&2; \
            echo -e "${C_BOLD}${C_BLUE}  $*${C_RESET}" >&2; \
            echo -e "${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}\n" >&2; }

# Test program source directory
TEST_PROGRAMS_DIR="${SCRIPT_DIR}/test_program"

# Per-test default timeout (seconds)
DEFAULT_TEST_TIMEOUT=30

# These arrays are populated by discover_tests()
declare -a TEST_NAMES=()      # binary name  (e.g. "test_brk")
declare -a TEST_SRCS=()       # source path  (e.g. "test_program/test_brk.c")
declare -a TEST_LDFLAGS=()    # extra link flags (e.g. "-lpthread")
declare -a TEST_TIMEOUTS=()   # per-test timeout

# Musl-based cross-compiler per architecture
# Toolchain layout: <root>/<arch>-linux-musl-cross/bin/<arch>-linux-musl-gcc
# Will be auto-detected across multiple search paths by detect_musl_cross_cc()
declare -A CROSS_CC=(
    [x86_64]="x86_64-linux-musl-gcc"
    [riscv64]="riscv64-linux-musl-gcc"
)

# Phase 1.5: Linux QEMU system-mode configuration
LINUX_KERNEL_VERSION="${LINUX_KERNEL_VERSION:-6.12.7}"
LINUX_KERNEL_DIR="${SCRIPT_DIR}/linux-sourcecode"
BUSYBOX_BUILD_DIR="${SCRIPT_DIR}/busybox-build"
PHASE15_ROOTFS_SIZE=128
PHASE15_ARCH="${HOST_ARCH}"
declare -A PHASE15_PASS PHASE15_FAIL PHASE15_SKIP

# QEMU user-mode emulator per architecture (for Linux cross-arch testing)
declare -A QEMU_USER=(
    [riscv64]="qemu-riscv64-static"
    [aarch64]="qemu-aarch64-static"
    [loongarch64]="qemu-loongarch64-static"
)

# StarryOS target triple per architecture
declare -A STARRY_TARGET=(
    [riscv64]="riscv64gc-unknown-none-elf"
    [aarch64]="aarch64-unknown-none-softfloat"
    [x86_64]="x86_64-unknown-none"
    [loongarch64]="loongarch64-unknown-none-softfloat"
)

# Directories
BUILD_DIR="${SCRIPT_DIR}/build"
LOG_DIR="${SCRIPT_DIR}/logs"

# Result tracking
declare -A LINUX_PASS LINUX_FAIL LINUX_SKIP
STARRY_TOTAL_PASS=0; STARRY_TOTAL_FAIL=0; STARRY_TOTAL_SKIP=0

# ============================================================
#  Argument Parsing
# ============================================================

RUN_PHASE1=false     # Phase 1 (bare/user-mode) — opt-in
RUN_PHASE15=true     # Phase 1.5 (QEMU system-mode Linux) — default
RUN_STARRY=true      # Phase 2 (StarryOS) — default
SKIP_DEP_CHECK=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bare|--user-mode) RUN_PHASE1=true; shift ;;
        --linux-only)       RUN_PHASE15=true; RUN_STARRY=false; shift ;;
        --starry-only)      RUN_PHASE15=false; RUN_STARRY=true; shift ;;
        --linux-arch)       PHASE15_ARCH="$2"; shift 2 ;;
        --starry-arch)      STARRY_ARCH="$2"; shift 2 ;;
        --tgoskits)         TGOSKITS_DIR="$2"; shift 2 ;;
        --no-dep-check)     SKIP_DEP_CHECK=true; shift ;;
        -h|--help)
            sed -n '2,/^$/s/^# \?//p' "$0"
            exit 0 ;;
        *) err "Unknown option: $1"; exit 1 ;;
    esac
done

# Auto-detect tgoskits directory
if [[ -z "$TGOSKITS_DIR" ]]; then
    for candidate in "${SCRIPT_DIR}/../tgoskits" "${SCRIPT_DIR}/../TgoSkits" \
                     "${SCRIPT_DIR}/tgoskits" "${SCRIPT_DIR}/../../tgoskits"; do
        if [[ -d "$candidate" && -f "$candidate/Cargo.toml" ]]; then
            TGOSKITS_DIR="$(cd "$candidate" && pwd)"
            break
        fi
    done
fi

# ============================================================
#  Dependency Helpers (reproducible)
# ============================================================

# Auto-detect musl cross-compilers across multiple possible install locations
detect_musl_cross_cc() {
    local -a search_roots=(
        "${SCRIPT_DIR}/musl-cross"
        "${SCRIPT_DIR}/../musl-cross"
        "$HOME/package"
        "$HOME/musl-cross"
        "/usr/local/musl-cross"
        "/opt/musl-cross"
    )

    for root in "${search_roots[@]}"; do
        for arch in "${!CROSS_CC[@]}"; do
            local cc="${root}/${arch}-linux-musl-cross/bin/${arch}-linux-musl-gcc"
            if [[ -x "$cc" ]]; then
                CROSS_CC[$arch]="$cc"
            fi
        done
    done
}

# Ensure a static busybox binary is available.
# Search order: system busybox-static → previously compiled → auto-download+compile
ensure_busybox() {
    local output="$1"   # output path for the busybox binary

    # 1. Check system busybox (static)
    for candidate in /usr/bin/busybox /bin/busybox; do
        if [[ -x "$candidate" ]] && file "$candidate" | grep -q "statically linked"; then
            ln -sf "$candidate" "$output"
            ok "Using system busybox: $candidate"
            return 0
        fi
    done

    # 2. Check previously compiled busybox
    if [[ -x "${BUSYBOX_BUILD_DIR}/busybox" ]]; then
        ln -sf "${BUSYBOX_BUILD_DIR}/busybox" "$output"
        ok "Using previously built busybox"
        return 0
    fi

    # 3. Auto-download and compile
    info "Busybox not found. Downloading and compiling static build ..."
    mkdir -p "$BUSYBOX_BUILD_DIR"
    local bb_version="1.36.1"
    local bb_url="https://busybox.net/downloads/busybox-${bb_version}.tar.bz2"
    local bb_tar="${BUSYBOX_BUILD_DIR}/busybox-${bb_version}.tar.bz2"

    if [[ ! -f "$bb_tar" ]]; then
        curl -fSL -o "$bb_tar" "$bb_url" || { err "Failed to download busybox"; return 1; }
    fi

    local bb_src="${BUSYBOX_BUILD_DIR}/busybox-${bb_version}"
    if [[ ! -d "$bb_src" ]]; then
        tar -xf "$bb_tar" -C "$BUSYBOX_BUILD_DIR" || { err "Failed to extract busybox"; return 1; }
    fi

    (
        cd "$bb_src"
        make defconfig 1>/dev/null 2>&1
        sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
        make -j"$(nproc)" 1>/dev/null 2>&1
        cp busybox "${BUSYBOX_BUILD_DIR}/busybox"
    ) || { err "Busybox build failed"; return 1; }

    ln -sf "${BUSYBOX_BUILD_DIR}/busybox" "$output"
    ok "Busybox compiled: ${BUSYBOX_BUILD_DIR}/busybox"
}

# ============================================================
#  Test Discovery
# ============================================================

discover_tests() {
    if [[ ! -d "$TEST_PROGRAMS_DIR" ]]; then
        err "Test program directory not found: $TEST_PROGRAMS_DIR"
        err "Expected: <repo>/test_program/*.c"
        exit 1
    fi

    local -a c_files=()
    while IFS= read -r -d '' f; do
        c_files+=("$f")
    done < <(find "$TEST_PROGRAMS_DIR" -maxdepth 1 -name '*.c' -type f -print0 | sort -z)

    if [[ ${#c_files[@]} -eq 0 ]]; then
        err "No .c files found in $TEST_PROGRAMS_DIR"
        exit 1
    fi

    info "Discovered ${#c_files[@]} test program(s) in test_program/"

    for src_path in "${c_files[@]}"; do
        local src_file
        src_file="$(basename "$src_path")"
        local name="${src_file%.c}"

        TEST_NAMES+=("$name")
        TEST_SRCS+=("$src_path")          # full path now, not relative
        TEST_TIMEOUTS+=("$DEFAULT_TEST_TIMEOUT")

        # Auto-detect -lpthread: scan source for pthread usage
        if grep -qE '(pthread_create|pthread_join|pthread_mutex|pthread_cond|#include\s*<pthread)' "$src_path" 2>/dev/null; then
            TEST_LDFLAGS+=("-lpthread")
            TEST_TIMEOUTS[-1]=120  # pthread tests tend to run longer
            info "  $src_file  (needs -lpthread)"
        else
            TEST_LDFLAGS+=("")
            info "  $src_file"
        fi
    done
}

# ============================================================
#  Cleanup
# ============================================================

# Tracks resources to clean up on exit
_CLEANUP_MOUNTS=()
_CLEANUP_FILES=()

cleanup() {
    # Unmount any leftover mounts
    for mp in "${_CLEANUP_MOUNTS[@]+"${_CLEANUP_MOUNTS[@]}"}"; do
        if mountpoint -q "$mp" 2>/dev/null; then
            sudo umount "$mp" 2>/dev/null || true
        fi
        rmdir "$mp" 2>/dev/null || true
    done
    # Remove temp files
    for f in "${_CLEANUP_FILES[@]+"${_CLEANUP_FILES[@]}"}"; do
        rm -f "$f" 2>/dev/null || true
    done
}
trap cleanup EXIT

# ============================================================
#  Prerequisites Check
# ============================================================

check_prerequisites() {
    header "Checking Prerequisites"

    local missing=()

    # Basic tools
    for cmd in gcc make; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done

    # Detect musl cross-compilers
    detect_musl_cross_cc

    for arch in "${!CROSS_CC[@]}"; do
        local cc="${CROSS_CC[$arch]}"
        if [[ -x "$cc" ]]; then
            ok "musl CC for $arch: $cc"
        else
            warn "musl CC for $arch not found: $cc"
            unset CROSS_CC[$arch]
        fi
    done

    if [[ ${#CROSS_CC[@]} -eq 0 ]]; then
        missing+=("musl cross-compiler (install musl-cross-make or set CROSS_CC paths)")
    fi

    # QEMU user-mode for Phase 1 cross-arch testing
    if $RUN_PHASE1; then
        for arch in "${!QEMU_USER[@]}"; do
            if command -v "${QEMU_USER[$arch]}" &>/dev/null; then
                : # available
            elif command -v "${QEMU_USER[$arch]/-static/}" &>/dev/null; then
                QEMU_USER[$arch]="${QEMU_USER[$arch]/-static/}"
            else
                warn "QEMU user-mode for $arch not found — that arch will be skipped"
            fi
        done
    fi

    # Phase 1.5 prerequisites
    if $RUN_PHASE15; then
        local qemu_sys="qemu-system-${PHASE15_ARCH}"
        if ! command -v "$qemu_sys" &>/dev/null; then
            missing+=("$qemu_sys (install: sudo apt install qemu-system-misc)")
        fi

        if ! command -v debugfs &>/dev/null; then
            missing+=("debugfs (install: sudo apt install e2fsprogs)")
        fi

        if ! command -v mkfs.ext4 &>/dev/null; then
            missing+=("mkfs.ext4 (install: sudo apt install e2fsprogs)")
        fi

        # Kernel build tools (only if kernel not already compiled)
        local kernel_image=""
        case "$PHASE15_ARCH" in
            x86_64)  kernel_image="${LINUX_KERNEL_DIR}/linux-${LINUX_KERNEL_VERSION}/arch/x86/boot/bzImage" ;;
            riscv64) kernel_image="${LINUX_KERNEL_DIR}/linux-${LINUX_KERNEL_VERSION}/arch/riscv/boot/Image" ;;
        esac
        if [[ ! -f "$kernel_image" ]]; then
            for cmd in flex bison bc; do
                command -v "$cmd" &>/dev/null || missing+=("$cmd (install: sudo apt install flex bison bc)")
            done
            if [[ ! -f /usr/include/libelf.h ]] || [[ ! -f /usr/include/gelf.h ]]; then
                missing+=("libelf-dev (install: sudo apt install libelf-dev)")
            fi
        fi
    fi

    # Phase 2 (StarryOS) prerequisites — no longer needs sudo
    if $RUN_STARRY; then
        command -v cargo &>/dev/null || missing+=("cargo (Rust toolchain)")

        local qemu_sys="qemu-system-${STARRY_ARCH}"
        if ! command -v "$qemu_sys" &>/dev/null; then
            missing+=("$qemu_sys (install: sudo apt install qemu-system-misc)")
        fi

        if ! command -v debugfs &>/dev/null; then
            missing+=("debugfs (install: sudo apt install e2fsprogs)")
        fi

        if [[ -z "$TGOSKITS_DIR" ]]; then
            missing+=("tgoskits repo (use --tgoskits or set TGOSKITS_DIR)")
        elif [[ ! -f "$TGOSKITS_DIR/Cargo.toml" ]]; then
            err "$TGOSKITS_DIR doesn't look like the tgoskits repo"
            exit 1
        fi
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "Missing prerequisites:"
        printf '    - %s\n' "${missing[@]}"
        exit 1
    fi

    ok "Prerequisites OK"
    if [[ -n "$TGOSKITS_DIR" ]]; then
        info "tgoskits: $TGOSKITS_DIR"
    fi
    info "host arch: $HOST_ARCH"
}

# ============================================================
#  Phase 1: Linux Multi-Architecture Tests
# ============================================================

phase1_linux_tests() {
    header "Phase 1: Linux Multi-Architecture Tests"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    # Determine which architectures we can test
    local -a test_arches=()
    for arch in "${!CROSS_CC[@]}"; do
        local cc="${CROSS_CC[$arch]}"
        if [[ ! -x "$cc" ]]; then
            warn "Skipping $arch: $cc not found"
            continue
        fi
        if [[ "$arch" == "$HOST_ARCH" ]]; then
            test_arches+=("$arch")
        else
            # Non-native arch also needs QEMU user-mode
            local qemu="${QEMU_USER[$arch]:-}"
            if [[ -n "$qemu" ]] && command -v "$qemu" &>/dev/null; then
                test_arches+=("$arch")
            else
                warn "Skipping $arch: compiler found but QEMU user-mode not found"
            fi
        fi
    done

    if [[ ${#test_arches[@]} -eq 0 ]]; then
        err "No architectures available for Linux testing"
        return 1
    fi

    info "Will test on: ${test_arches[*]}"

    # Test each architecture
    for arch in "${test_arches[@]}"; do
        section "Linux/$arch"
        local arch_build="${BUILD_DIR}/linux_${arch}"
        local arch_log="${LOG_DIR}/linux_${arch}"
        mkdir -p "$arch_build"

        # Select compiler
        local cc="${CROSS_CC[$arch]}"

        # --- Compile ---
        local compile_fail=0
        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            local src_path="${TEST_SRCS[$i]}"
            local ldflags="${TEST_LDFLAGS[$i]}"

            if [[ ! -f "$src_path" ]]; then
                err "Source not found: $src_path"
                compile_fail=1
                continue
            fi

            info "Compiling $name ($arch) ..."
            if $cc -static -Wall -Wextra -O2 -o "${arch_build}/${name}" \
                   "$src_path" $ldflags 2>"${arch_log}_${name}_compile.log"; then
                ok "Built $name"
            else
                err "Compile failed: $name ($arch)"
                cat "${arch_log}_${name}_compile.log"
                compile_fail=1
            fi
        done

        if [[ $compile_fail -ne 0 ]]; then
            warn "Some compilations failed for $arch, skipping execution"
            continue
        fi

        # --- Run ---
        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            local binary="${arch_build}/${name}"
            local test_timeout="${TEST_TIMEOUTS[$i]}"

            if [[ ! -f "$binary" ]]; then
                warn "SKIP $name ($arch): binary not found"
                LINUX_SKIP[$arch]=$((${LINUX_SKIP[$arch]:-0} + 1))
                continue
            fi

            info "Running $name on Linux/$arch (timeout: ${test_timeout}s) ..."

            # Build run command
            local run_cmd
            if [[ "$arch" == "$HOST_ARCH" ]]; then
                run_cmd="$binary"
            else
                run_cmd="${QEMU_USER[$arch]} $binary"
            fi

            local log_file="${arch_log}_${name}_run.log"
            local exit_code=0
            timeout "$test_timeout" $run_cmd >"$log_file" 2>&1 || exit_code=$?

            if [[ $exit_code -eq 0 ]]; then
                ok "PASS  $name ($arch)"
                LINUX_PASS[$arch]=$((${LINUX_PASS[$arch]:-0} + 1))
            elif [[ $exit_code -eq 124 ]]; then
                err "TIMEOUT $name ($arch)"
                LINUX_FAIL[$arch]=$((${LINUX_FAIL[$arch]:-0} + 1))
            else
                err "FAIL  $name ($arch) — exit code $exit_code"
                LINUX_FAIL[$arch]=$((${LINUX_FAIL[$arch]:-0} + 1))
                tail -20 "$log_file"
            fi
            echo ""
        done
    done
}

# ============================================================
#  Phase 1.5 Helpers
# ============================================================

download_and_build_kernel() {
    local arch="$1"
    local kernel_url="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_KERNEL_VERSION}.tar.xz"
    local kernel_tar="${LINUX_KERNEL_DIR}/linux-${LINUX_KERNEL_VERSION}.tar.xz"
    local kernel_src="${LINUX_KERNEL_DIR}/linux-${LINUX_KERNEL_VERSION}"

    mkdir -p "$LINUX_KERNEL_DIR"

    # Determine expected kernel image path
    local kernel_image=""
    case "$arch" in
        x86_64)  kernel_image="${kernel_src}/arch/x86/boot/bzImage" ;;
        riscv64) kernel_image="${kernel_src}/arch/riscv/boot/Image" ;;
        *)
            err "Unsupported arch for kernel build: $arch"
            return 1 ;;
    esac

    # Cache: skip if already built
    if [[ -f "$kernel_image" ]]; then
        ok "Linux kernel already compiled: $kernel_image"
        echo "$kernel_image"
        return 0
    fi

    # Download
    if [[ ! -f "$kernel_tar" ]]; then
        info "Downloading Linux kernel ${LINUX_KERNEL_VERSION} ..."
        curl -fSL --silent --show-error -o "$kernel_tar" "$kernel_url" || { err "Failed to download kernel"; return 1; }
        ok "Downloaded kernel source"
    fi

    # Extract
    if [[ ! -d "$kernel_src" ]]; then
        info "Extracting kernel source ..."
        tar -xf "$kernel_tar" -C "$LINUX_KERNEL_DIR" || { err "Failed to extract kernel"; return 1; }
    fi

    # Configure
    info "Configuring Linux kernel for $arch ..."
    (
        cd "$kernel_src"

        local cross_prefix=""
        if [[ "$arch" != "$HOST_ARCH" ]]; then
            cross_prefix="${arch}-linux-gnu-"
        fi

        # Start with defconfig
        if [[ "$arch" != "$HOST_ARCH" ]]; then
            make ARCH="$arch" CROSS_COMPILE="$cross_prefix" defconfig 1>/dev/null 2>&1
        else
            make defconfig 1>/dev/null 2>&1
        fi

        # Append essential config for QEMU system-mode testing
        cat >> .config << 'KERNELEOF'
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_EXT4_FS=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_TTY=y
CONFIG_PRINTK=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
KERNELEOF

        if [[ "$arch" != "$HOST_ARCH" ]]; then
            make ARCH="$arch" CROSS_COMPILE="$cross_prefix" olddefconfig 1>/dev/null 2>&1
        else
            make olddefconfig 1>/dev/null 2>&1
        fi
    ) || { err "Kernel configuration failed"; return 1; }

    # Build
    info "Building Linux kernel (this takes several minutes, output suppressed) ..."
    local build_log="${LINUX_KERNEL_DIR}/build_${arch}.log"
    (
        cd "$kernel_src"

        if [[ "$arch" != "$HOST_ARCH" ]]; then
            local cross_prefix="${arch}-linux-gnu-"
            make -j"$(nproc)" ARCH="$arch" CROSS_COMPILE="$cross_prefix"
        else
            make -j"$(nproc)"
        fi
    ) >"$build_log" 2>&1 || {
        err "Kernel build failed. Last 20 lines:"
        tail -20 "$build_log"
        return 1
    }

    if [[ -f "$kernel_image" ]]; then
        ok "Kernel built: $kernel_image"
        echo "$kernel_image"
    else
        err "Kernel image not found after build"
        return 1
    fi
}

create_rootfs_image() {
    local arch="$1"
    local build_dir="$2"
    local output_rootfs="$3"
    local rootfs_size_mb="${PHASE15_ROOTFS_SIZE:-128}"

    info "Creating ${rootfs_size_mb}MB ext4 rootfs at $output_rootfs ..."

    # Step 1: Create empty ext4 image
    dd if=/dev/zero of="$output_rootfs" bs=1M count="$rootfs_size_mb" status=none
    mkfs.ext4 -F -q "$output_rootfs"

    # Step 2: Ensure busybox is available
    local busybox_bin="${build_dir}/busybox"
    ensure_busybox "$busybox_bin" || { err "Cannot obtain busybox"; return 1; }

    # Resolve symlink if busybox is a symlink
    busybox_bin="$(readlink -f "$busybox_bin")"

    # Step 3: Build debugfs command script
    local cmd_file
    cmd_file=$(mktemp /tmp/debugfs_cmds.XXXXXX)
    _CLEANUP_FILES+=("$cmd_file")

    # Directory structure
    cat > "$cmd_file" << 'ROOTFS_CMDS'
mkdir bin
mkdir sbin
mkdir dev
mkdir proc
mkdir sys
mkdir tmp
mkdir etc
mkdir root
mkdir mnt
mkdir var
mkdir lib
ROOTFS_CMDS

    # Inject busybox
    echo "write ${busybox_bin} /bin/busybox" >> "$cmd_file"

    # Create busybox symlinks for common applets
    for applet in sh ash cat ls mount umount mkdir rm cp mv ln echo printf test \
                  sleep timeout basename dirname grep sed awk sort head tail wc \
                  tr cut tee poweroff reboot halt init mknod chmod chown; do
        echo "symlink /bin/${applet} /bin/busybox" >> "$cmd_file"
    done

    # Inject compiled test binaries
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local binary="${build_dir}/${name}"
        if [[ -f "$binary" ]]; then
            echo "write ${binary} /bin/${name}" >> "$cmd_file"
        fi
    done

    # Create init script
    local init_script
    init_script=$(mktemp /tmp/init_script.XXXXXX)
    _CLEANUP_FILES+=("$init_script")

    local test_bin_list=""
    for i in "${!TEST_NAMES[@]}"; do
        test_bin_list+="/bin/${TEST_NAMES[$i]} "
    done

    cat > "$init_script" << 'INIT_EOF'
#!/bin/sh
# Phase 1.5 init script — auto-run inside QEMU system-mode Linux

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true

echo ""
echo "========================================="
echo "  Linux QEMU System-Mode Test Suite"
echo "  Kernel: $(uname -r)"
echo "  Arch:   $(uname -m)"
echo "========================================="
echo ""

PER_TEST_TIMEOUT=30
TOTAL=0
PASSED=0
FAILED=0

for test_bin in TEST_BIN_PLACEHOLDER; do
    if [ -x "$test_bin" ]; then
        echo "--- Running $(basename $test_bin) ---"
        TOTAL=$((TOTAL + 1))
        timeout $PER_TEST_TIMEOUT "$test_bin"
        rc=$?
        if [ $rc -eq 0 ]; then
            PASSED=$((PASSED + 1))
            echo "--- $(basename $test_bin): PASSED ---"
        elif [ $rc -eq 127 ] 2>/dev/null; then
            "$test_bin"
            rc=$?
            if [ $rc -eq 0 ]; then
                PASSED=$((PASSED + 1))
                echo "--- $(basename $test_bin): PASSED ---"
            else
                FAILED=$((FAILED + 1))
                echo "--- $(basename $test_bin): FAILED (exit=$rc) ---"
            fi
        elif [ $rc -ge 128 ]; then
            FAILED=$((FAILED + 1))
            echo "--- $(basename $test_bin): TIMEOUT/KILLED (exit=$rc) ---"
        else
            FAILED=$((FAILED + 1))
            echo "--- $(basename $test_bin): FAILED (exit=$rc) ---"
        fi
        echo ""
    else
        echo "--- SKIP $(basename $test_bin) (not found) ---"
    fi
done

echo "========================================="
echo "  RESULTS: $PASSED/$TOTAL passed, $FAILED failed"
echo "========================================="

echo "Tests complete. Powering off..."
poweroff -f
INIT_EOF

    # Replace placeholder with actual test binary list
    sed -i "s|TEST_BIN_PLACEHOLDER|${test_bin_list}|" "$init_script"

    # Make init script executable before writing to rootfs
    chmod +x "$init_script"
    echo "write ${init_script} /init" >> "$cmd_file"

    # Create /etc/passwd and /etc/group
    local passwd_file
    passwd_file=$(mktemp /tmp/passwd.XXXXXX)
    _CLEANUP_FILES+=("$passwd_file")
    echo "root:x:0:0:root:/root:/bin/sh" > "$passwd_file"
    echo "write ${passwd_file} /etc/passwd" >> "$cmd_file"

    local group_file
    group_file=$(mktemp /tmp/group.XXXXXX)
    _CLEANUP_FILES+=("$group_file")
    echo "root:x:0:" > "$group_file"
    echo "write ${group_file} /etc/group" >> "$cmd_file"

    # Step 4: Execute debugfs commands
    info "Injecting files into rootfs via debugfs ..."
    debugfs -w -f "$cmd_file" "$output_rootfs" 2>&1 | grep -viE "^(debugfs|Allocated inode|ext2fs_|symlink:)" || true

    # Step 5: Set permissions via modify_inode
    debugfs -w -R "modify_inode /init mode 0100755" "$output_rootfs" 2>/dev/null || true
    debugfs -w -R "modify_inode /bin/busybox mode 0100755" "$output_rootfs" 2>/dev/null || true
    for i in "${!TEST_NAMES[@]}"; do
        debugfs -w -R "modify_inode /bin/${TEST_NAMES[$i]} mode 0100755" "$output_rootfs" 2>/dev/null || true
    done
    debugfs -w -R "modify_inode /tmp mode 01777" "$output_rootfs" 2>/dev/null || true
    debugfs -w -R "modify_inode /root mode 01750" "$output_rootfs" 2>/dev/null || true
    debugfs -w -R "modify_inode /var mode 0755" "$output_rootfs" 2>/dev/null || true

    ok "Rootfs created: $output_rootfs"
}

# ============================================================
#  Phase 1.5: Linux QEMU System-Mode Tests
# ============================================================

phase15_linux_qemu_tests() {
    header "Phase 1.5: Linux QEMU System-Mode Tests ($PHASE15_ARCH)"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    local phase15_build="${BUILD_DIR}/linux_qemu_${PHASE15_ARCH}"
    local phase15_log="${LOG_DIR}/linux_qemu_${PHASE15_ARCH}"
    mkdir -p "$phase15_build"

    # ---- Step 1: Build Linux kernel ----
    section "Step 1/4: Build Linux Kernel ($PHASE15_ARCH)"

    local kernel_image
    kernel_image=$(download_and_build_kernel "$PHASE15_ARCH") || {
        err "Kernel build failed"
        return 1
    }

    # ---- Step 2: Compile test binaries ----
    section "Step 2/4: Compile Test Binaries ($PHASE15_ARCH)"

    local cc="${CROSS_CC[$PHASE15_ARCH]:-}"
    if [[ -z "$cc" ]] || [[ ! -x "$cc" ]]; then
        err "Compiler for $PHASE15_ARCH not found"
        return 1
    fi

    local compile_fail=0
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local src_path="${TEST_SRCS[$i]}"
        local ldflags="${TEST_LDFLAGS[$i]}"

        info "Compiling $name ($PHASE15_ARCH, static) ..."
        if $cc -static -Wall -Wextra -O2 -o "${phase15_build}/${name}" \
               "$src_path" $ldflags 2>"${phase15_log}_${name}_compile.log"; then
            ok "Built $name"
        else
            err "Compile failed: $name"
            cat "${phase15_log}_${name}_compile.log"
            compile_fail=1
        fi
    done

    if [[ $compile_fail -ne 0 ]]; then
        err "Some compilations failed for $PHASE15_ARCH"
        return 1
    fi

    # ---- Step 3: Create rootfs ----
    section "Step 3/4: Create ext4 Rootfs"

    local rootfs_img="${phase15_build}/rootfs.img"
    _CLEANUP_FILES+=("$rootfs_img")

    create_rootfs_image "$PHASE15_ARCH" "$phase15_build" "$rootfs_img"

    # ---- Step 4: Boot QEMU and capture results ----
    section "Step 4/4: Run Tests in QEMU System-Mode"

    local qemu_output="${phase15_log}_qemu_output.log"
    local phase15_timeout="${STARRY_TIMEOUT:-120}"

    info "Launching QEMU system-mode (timeout: ${phase15_timeout}s) ..."
    info "Kernel: $kernel_image"
    info "Rootfs: $rootfs_img"

    # Use -serial file:... to write serial output directly to log file,
    # avoiding pipe/tee issues where QEMU withholds output when stdout is not a terminal.
    # Truncate log file before QEMU starts
    : > "$qemu_output"

    set +e
    case "$PHASE15_ARCH" in
        x86_64)
            timeout "$phase15_timeout" qemu-system-x86_64 \
                -accel tcg \
                -kernel "$kernel_image" \
                -drive "file=${rootfs_img},format=raw,if=virtio" \
                -append "root=/dev/vda rw console=ttyS0 panic=-1 init=/init" \
                -serial "file:$qemu_output" -display none -no-reboot \
                -m 512M -smp 1
            ;;
        riscv64)
            timeout "$phase15_timeout" qemu-system-riscv64 \
                -machine virt \
                -accel tcg \
                -kernel "$kernel_image" \
                -drive "file=${rootfs_img},format=raw,if=virtio" \
                -append "root=/dev/vda rw console=ttyS0 panic=-1 init=/init" \
                -serial "file:$qemu_output" -display none -no-reboot \
                -m 512M -smp 1
            ;;
        *)
            err "Unsupported QEMU system arch: $PHASE15_ARCH"
            set -e
            return 1
            ;;
    esac
    local qemu_exit=$?
    set -e

    info "[DEBUG] QEMU process exit code: $qemu_exit (124=timeout, 137=killed)"

    # ---- Parse results ----
    section "Phase 1.5 Test Results ($PHASE15_ARCH)"

    if [[ $qemu_exit -eq 124 ]]; then
        warn "QEMU timed out after ${phase15_timeout}s"
    fi

    if [[ ! -s "$qemu_output" ]]; then
        err "No QEMU output captured"
        return 1
    fi

    # Parse results — same pattern as Phase 2
    local results_line
    results_line=$(grep -oP 'RESULTS: \d+/\d+ passed, \d+ failed' "$qemu_output" 2>/dev/null || true)

    if [[ -n "$results_line" ]]; then
        info "$results_line"

        local passed total failed
        passed=$(echo "$results_line" | grep -oP '\d+(?=/)' || echo "0")
        total=$(echo "$results_line" | grep -oP '\d+(?= passed)' || echo "0")
        failed=$(echo "$results_line" | grep -oP '\d+(?= failed)' || echo "0")

        PHASE15_PASS[$PHASE15_ARCH]=$passed
        PHASE15_FAIL[$PHASE15_ARCH]=$failed
        PHASE15_SKIP[$PHASE15_ARCH]=$(( ${#TEST_NAMES[@]} - total ))

        if [[ "$failed" -eq 0 ]] && [[ "$total" -gt 0 ]]; then
            ok "All Phase 1.5 tests PASSED ($passed/$total)"
        else
            err "Some Phase 1.5 tests FAILED ($failed/$total)"
        fi
    else
        warn "Could not parse summary from output — checking individual tests"

        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            if grep -q "--- Running ${name} ---" "$qemu_output" 2>/dev/null; then
                if grep -q "--- ${name}: PASSED ---" "$qemu_output" 2>/dev/null; then
                    ok "Phase 1.5/$PHASE15_ARCH: $name PASSED"
                    PHASE15_PASS[$PHASE15_ARCH]=$(( ${PHASE15_PASS[$PHASE15_ARCH]:-0} + 1 ))
                elif grep -q "--- ${name}: FAILED ---" "$qemu_output" 2>/dev/null; then
                    err "Phase 1.5/$PHASE15_ARCH: $name FAILED"
                    PHASE15_FAIL[$PHASE15_ARCH]=$(( ${PHASE15_FAIL[$PHASE15_ARCH]:-0} + 1 ))
                else
                    warn "Phase 1.5/$PHASE15_ARCH: $name — unknown result"
                    PHASE15_SKIP[$PHASE15_ARCH]=$(( ${PHASE15_SKIP[$PHASE15_ARCH]:-0} + 1 ))
                fi
            else
                warn "Phase 1.5/$PHASE15_ARCH: $name — not executed"
                PHASE15_SKIP[$PHASE15_ARCH]=$(( ${PHASE15_SKIP[$PHASE15_ARCH]:-0} + 1 ))
            fi
        done
    fi

    info "Full QEMU log: $qemu_output"
}

# ============================================================
#  Phase 2: StarryOS Tests
# ============================================================

phase2_starryos_tests() {
    header "Phase 2: StarryOS Tests ($STARRY_ARCH)"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    local starry_build="${BUILD_DIR}/starry_${STARRY_ARCH}"
    local starry_log="${LOG_DIR}/starry_${STARRY_ARCH}"
    mkdir -p "$starry_build"

    local cc="${CROSS_CC[$STARRY_ARCH]:-}"
    if [[ -z "$cc" ]] || ! command -v "$cc" &>/dev/null; then
        err "Cross-compiler for $STARRY_ARCH not found"
        err "Install: sudo apt install gcc-${STARRY_ARCH}-linux-gnu"
        return 1
    fi

    # ----------------------------------------------------------
    #  Step 1: Cross-compile test programs as static binaries
    # ----------------------------------------------------------
    section "Step 1/4: Cross-compile for StarryOS/$STARRY_ARCH"

    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local src_path="${TEST_SRCS[$i]}"
        local ldflags="${TEST_LDFLAGS[$i]}"

        info "Compiling $name ($STARRY_ARCH, static) ..."
        if $cc -static -Wall -Wextra -O2 -o "${starry_build}/${name}" \
               "$src_path" $ldflags 2>"${starry_log}_${name}_compile.log"; then
            ok "Built $name for StarryOS"
        else
            err "Compile failed: $name"
            cat "${starry_log}_${name}_compile.log"
            return 1
        fi
    done

    # ----------------------------------------------------------
    #  Step 2: Download rootfs and inject test binaries
    # ----------------------------------------------------------
    section "Step 2/4: Prepare rootfs with test binaries"

    info "Downloading rootfs (if not cached) ..."
    (
        cd "$TGOSKITS_DIR"
        cargo starry rootfs --arch "$STARRY_ARCH"
    )
    ok "Rootfs downloaded"

    local target_triple="${STARRY_TARGET[$STARRY_ARCH]}"
    local rootfs="${TGOSKITS_DIR}/target/${target_triple}/rootfs-${STARRY_ARCH}.img"

    if [[ ! -f "$rootfs" ]]; then
        err "Rootfs not found: $rootfs"
        return 1
    fi

    # Make a working copy so we don't corrupt the cached original
    local work_rootfs="${starry_build}/rootfs-${STARRY_ARCH}.img"
    info "Creating working copy of rootfs ..."
    cp "$rootfs" "$work_rootfs"
    _CLEANUP_FILES+=("$work_rootfs")
    ok "Working copy: $work_rootfs"

    # Resize to fit test binaries (~64 MB extra)
    info "Resizing rootfs ..."
    truncate -s +64M "$work_rootfs"
    e2fsck -fy "$work_rootfs" &>/dev/null || true
    resize2fs "$work_rootfs" &>/dev/null
    ok "Resized"

    # Inject test binaries via debugfs (no mount/sudo required)
    info "Injecting test binaries into rootfs via debugfs ..."

    # Build debugfs command script
    local cmd_file
    cmd_file=$(mktemp /tmp/starry_debugfs_cmds.XXXXXX)
    _CLEANUP_FILES+=("$cmd_file")

    # Write test binaries
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        echo "write ${starry_build}/${name} /bin/${name}" >> "$cmd_file"
    done

    # Create the test runner script
    local runner_script
    runner_script=$(mktemp /tmp/starry_runner.XXXXXX)
    _CLEANUP_FILES+=("$runner_script")

    cat > "$runner_script" << 'RUNNER'
#!/bin/sh
# ──────────────────────────────────────────────
#  Automated Syscall Test Runner for StarryOS
# ──────────────────────────────────────────────

PER_TEST_TIMEOUT=20

echo ""
echo "========================================="
echo "  StarryOS Syscall Test Suite"
echo "========================================="
echo ""

TOTAL=0
PASSED=0
FAILED=0

for test_bin in "$@"; do
    if [ -x "$test_bin" ]; then
        echo "--- Running $(basename $test_bin) ---"
        TOTAL=$((TOTAL + 1))
        timeout $PER_TEST_TIMEOUT "$test_bin"
        rc=$?
        if [ $rc -eq 0 ]; then
            PASSED=$((PASSED + 1))
            echo "--- $(basename $test_bin): PASSED ---"
        elif [ $rc -eq 127 ] && [ "$(basename $test_bin)" = "timeout" ]; then
            # timeout command not available — run without timeout
            "$test_bin"
            rc=$?
            if [ $rc -eq 0 ]; then
                PASSED=$((PASSED + 1))
                echo "--- $(basename $test_bin): PASSED ---"
            else
                FAILED=$((FAILED + 1))
                echo "--- $(basename $test_bin): FAILED (exit=$rc) ---"
            fi
        elif [ $rc -ge 128 ]; then
            FAILED=$((FAILED + 1))
            echo "--- $(basename $test_bin): TIMEOUT/KILLED (exit=$rc) ---"
        else
            FAILED=$((FAILED + 1))
            echo "--- $(basename $test_bin): FAILED (exit=$rc) ---"
        fi
        echo ""
    else
        echo "--- SKIP $(basename $test_bin) (not found) ---"
    fi
done

echo "========================================="
echo "  RESULTS: $PASSED/$TOTAL passed, $FAILED failed"
echo "========================================="
RUNNER
    chmod +x "$runner_script"
    echo "write ${runner_script} /bin/run_syscall_tests.sh" >> "$cmd_file"

    # Build the argument list for .profile
    local runner_args=""
    for i in "${!TEST_NAMES[@]}"; do
        runner_args+="/bin/${TEST_NAMES[$i]} "
    done

    # Create /root/.profile
    local profile_script
    profile_script=$(mktemp /tmp/starry_profile.XXXXXX)
    _CLEANUP_FILES+=("$profile_script")

    cat > "$profile_script" << PROFILE
#!/bin/sh
echo ""
echo "[auto-test] Running syscall tests from .profile ..."
/bin/run_syscall_tests.sh ${runner_args}
echo ""
echo "[auto-test] Tests complete. Exiting shell."
exit 0
PROFILE
    chmod +x "$profile_script"
    echo "write ${profile_script} /root/.profile" >> "$cmd_file"

    # Execute all debugfs commands
    info "Writing files via debugfs ..."
    debugfs -w -f "$cmd_file" "$work_rootfs" 2>&1 | grep -viE "^(debugfs|Allocated inode|ext2fs_|symlink:)" || true

    # Set permissions via modify_inode
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        debugfs -w -R "modify_inode /bin/${name} mode 0100755" "$work_rootfs" 2>/dev/null || true
    done
    debugfs -w -R "modify_inode /bin/run_syscall_tests.sh mode 0100755" "$work_rootfs" 2>/dev/null || true
    debugfs -w -R "modify_inode /root/.profile mode 0100755" "$work_rootfs" 2>/dev/null || true
    debugfs -w -R "modify_inode /tmp mode 01777" "$work_rootfs" 2>/dev/null || true

    ok "Rootfs prepared via debugfs (no sudo required)"

    # ----------------------------------------------------------
    #  Step 3: Build StarryOS kernel
    # ----------------------------------------------------------
    section "Step 3/4: Build StarryOS kernel ($STARRY_ARCH)"

    info "Building StarryOS (this may take a while) ..."
    if ( cd "$TGOSKITS_DIR" && cargo starry build --arch "$STARRY_ARCH" ); then
        ok "StarryOS kernel built"
    else
        err "StarryOS kernel build failed"
        return 1
    fi

    # ----------------------------------------------------------
    #  Step 4: Run StarryOS in QEMU with injected rootfs
    # ----------------------------------------------------------
    section "Step 4/4: Run tests on StarryOS ($STARRY_ARCH)"

    # We need to run QEMU with our modified rootfs instead of the default one.
    # Strategy: temporarily replace the default rootfs with our working copy,
    # then run `cargo starry qemu`, then restore.
    info "Swapping rootfs with test-injected version ..."
    local original_rootfs_backup="${rootfs}.original_backup"
    cp "$rootfs" "$original_rootfs_backup"
    _CLEANUP_FILES+=("$original_rootfs_backup")
    cp "$work_rootfs" "$rootfs"

    local starry_output="${starry_log}_qemu_output.log"
    local qemu_exit_code=0

    info "Launching StarryOS in QEMU (timeout: ${STARRY_TIMEOUT}s) ..."
    info "Output is displayed in real-time and captured to: $starry_output"

    # Run StarryOS in QEMU with timeout.
    # tee shows output in real-time AND saves to log file.
    set +e
    ( cd "$TGOSKITS_DIR" && timeout "$STARRY_TIMEOUT" cargo starry qemu --arch "$STARRY_ARCH" ) \
        2>&1 | tee "$starry_output"
    qemu_exit_code=${PIPESTATUS[0]}
    set -e

    # Restore original rootfs
    info "Restoring original rootfs ..."
    cp "$original_rootfs_backup" "$rootfs"
    rm -f "$original_rootfs_backup"
    ok "Original rootfs restored"

    # ----------------------------------------------------------
    #  Parse results
    # ----------------------------------------------------------
    section "StarryOS Test Results"

    if [[ $qemu_exit_code -eq 124 ]]; then
        warn "QEMU timed out after ${STARRY_TIMEOUT}s"
        warn "Some tests may not have completed"
    fi

    if [[ ! -s "$starry_output" ]]; then
        err "No QEMU output captured"
        return 1
    fi

    # Split per-test logs from the combined QEMU output
    # (mirrors Linux side's per-test *_run.log structure)
    info "Splitting per-test logs ..."
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local per_test_log="${starry_log}_${name}_run.log"
        # QEMU output may contain binary bytes; use strings + sed to extract
        strings "$starry_output" | sed -n "/^--- Running ${name} ---$/,/^--- ${name}:/p" > "$per_test_log"
        if [[ -s "$per_test_log" ]]; then
            ok "Log saved: ${name}_run.log"
        else
            : > "$per_test_log"  # create empty log for consistency
        fi
    done

    # Display relevant output sections
    echo ""
    echo "--- StarryOS serial output (test-related) ---"
    # Extract everything between our test markers
    sed -n '/StarryOS Syscall Test Suite/,/RESULTS:/p' "$starry_output" || true
    echo "--- End of test output ---"
    echo ""

    # Parse results
    local results_line
    results_line=$(grep -oP 'RESULTS: \d+/\d+ passed, \d+ failed' "$starry_output" 2>/dev/null || true)
    if [[ -n "$results_line" ]]; then
        info "$results_line"

        # Extract counts
        local passed total failed
        passed=$(echo "$results_line" | grep -oP '\d+(?=/)' || echo "0")
        total=$(echo "$results_line" | grep -oP '\d+(?= passed)' || echo "0")
        failed=$(echo "$results_line" | grep -oP '\d+(?= failed)' || echo "0")

        STARRY_TOTAL_PASS=$passed
        STARRY_TOTAL_FAIL=$failed
        STARRY_TOTAL_SKIP=$(( ${#TEST_NAMES[@]} - total ))

        if [[ "$failed" -eq 0 ]] && [[ "$total" -gt 0 ]]; then
            ok "All StarryOS tests PASSED ($passed/$total)"
        else
            err "Some StarryOS tests FAILED ($failed/$total)"
        fi
    else
        warn "Could not parse test results from output"
        warn "Full log: $starry_output"

        # Fall back: look for individual test markers
        local found_any=false
        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            if grep -q "--- Running ${name} ---" "$starry_output" 2>/dev/null; then
                found_any=true
                if grep -q "--- ${name}: PASSED ---" "$starry_output" 2>/dev/null; then
                    ok "StarryOS: $name PASSED"
                    ((STARRY_TOTAL_PASS++))
                elif grep -q "--- ${name}: FAILED ---" "$starry_output" 2>/dev/null; then
                    err "StarryOS: $name FAILED"
                    ((STARRY_TOTAL_FAIL++))
                else
                    warn "StarryOS: $name — unknown result"
                    ((STARRY_TOTAL_SKIP++))
                fi
            else
                warn "StarryOS: $name — not executed"
                ((STARRY_TOTAL_SKIP++))
            fi
        done
    fi

    info "Full QEMU log: $starry_output"
}

# ============================================================
#  Summary
# ============================================================

print_summary() {
    header "Test Summary"

    if $RUN_PHASE1; then
        echo -e "${C_BOLD}Phase 1 — Linux User-Mode Tests (bare):${C_RESET}"
        local -A seen_arches=()
        for arch in "${!LINUX_PASS[@]}" "${!LINUX_FAIL[@]}" "${!LINUX_SKIP[@]}"; do
            [[ -n "${seen_arches[$arch]:-}" ]] && continue
            seen_arches[$arch]=1
            local p="${LINUX_PASS[$arch]:-0}"
            local f="${LINUX_FAIL[$arch]:-0}"
            local s="${LINUX_SKIP[$arch]:-0}"
            [[ $p -eq 0 && $f -eq 0 && $s -eq 0 ]] && continue
            printf "  %-12s " "$arch:"
            if [[ $f -eq 0 && $p -gt 0 ]]; then
                echo -e "${C_GREEN}${p} passed${C_RESET}, ${s} skipped"
            elif [[ $f -gt 0 ]]; then
                echo -e "${C_RED}${p} passed, ${f} FAILED${C_RESET}, ${s} skipped"
            else
                echo -e "${p} passed, ${f} failed, ${s} skipped"
            fi
        done
        echo ""
    fi

    if $RUN_PHASE15; then
        echo -e "${C_BOLD}Phase 1.5 — Linux QEMU System-Mode Tests:${C_RESET}"
        local -A seen_arches=()
        for arch in "${!PHASE15_PASS[@]}" "${!PHASE15_FAIL[@]}" "${!PHASE15_SKIP[@]}"; do
            [[ -n "${seen_arches[$arch]:-}" ]] && continue
            seen_arches[$arch]=1
            local p="${PHASE15_PASS[$arch]:-0}"
            local f="${PHASE15_FAIL[$arch]:-0}"
            local s="${PHASE15_SKIP[$arch]:-0}"
            [[ $p -eq 0 && $f -eq 0 && $s -eq 0 ]] && continue
            printf "  %-12s " "$arch:"
            if [[ $f -eq 0 && $p -gt 0 ]]; then
                echo -e "${C_GREEN}${p} passed${C_RESET}, ${s} skipped"
            elif [[ $f -gt 0 ]]; then
                echo -e "${C_RED}${p} passed, ${f} FAILED${C_RESET}, ${s} skipped"
            else
                echo -e "${p} passed, ${f} failed, ${s} skipped"
            fi
        done
        echo ""
    fi

    if $RUN_STARRY; then
        echo -e "${C_BOLD}Phase 2 — StarryOS Tests ($STARRY_ARCH):${C_RESET}"
        printf "  %-12s " "$STARRY_ARCH:"
        if [[ $STARRY_TOTAL_FAIL -eq 0 && $STARRY_TOTAL_PASS -gt 0 ]]; then
            echo -e "${C_GREEN}${STARRY_TOTAL_PASS} passed${C_RESET}, ${STARRY_TOTAL_SKIP} skipped"
        elif [[ $STARRY_TOTAL_FAIL -gt 0 ]]; then
            echo -e "${C_RED}${STARRY_TOTAL_PASS} passed, ${STARRY_TOTAL_FAIL} FAILED${C_RESET}, ${STARRY_TOTAL_SKIP} skipped"
        else
            echo -e "${STARRY_TOTAL_PASS} passed, ${STARRY_TOTAL_FAIL} failed, ${STARRY_TOTAL_SKIP} skipped"
        fi
        echo ""
    fi

    echo -e "Logs: ${LOG_DIR}/"
    echo -e "Build artifacts: ${BUILD_DIR}/"
    echo ""

    # Exit code
    local total_fail=0
    if $RUN_PHASE1; then
        for arch in x86_64 riscv64 aarch64; do
            total_fail=$(( total_fail + ${LINUX_FAIL[$arch]:-0} ))
        done
    fi
    if $RUN_PHASE15; then
        for arch in "${!PHASE15_FAIL[@]}"; do
            total_fail=$(( total_fail + ${PHASE15_FAIL[$arch]:-0} ))
        done
    fi
    if $RUN_STARRY; then
        total_fail=$(( total_fail + STARRY_TOTAL_FAIL ))
    fi

    if [[ $total_fail -eq 0 ]]; then
        ok "All tests PASSED"
        return 0
    else
        err "$total_fail test(s) FAILED"
        return 1
    fi
}

# ============================================================
#  Main
# ============================================================

main() {
    echo -e "${C_BOLD}╔══════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_BOLD}║       Syscall Test Runner — Linux & StarryOS    ║${C_RESET}"
    echo -e "${C_BOLD}╚══════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo "  Test repo:      $SCRIPT_DIR"
    echo "  StarryOS:       ${TGOSKITS_DIR:-<not set>}"
    echo "  Host arch:      $HOST_ARCH"
    echo "  StarryOS arch:  $STARRY_ARCH"
    echo "  Phase 1.5 arch: $PHASE15_ARCH"
    echo ""
    echo "  Phases enabled:"
    $RUN_PHASE1  && echo "    Phase 1   (Linux user-mode, bare)"
    $RUN_PHASE15 && echo "    Phase 1.5 (Linux QEMU system-mode)"
    $RUN_STARRY  && echo "    Phase 2   (StarryOS)"
    echo ""

    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    if ! $SKIP_DEP_CHECK; then
        check_prerequisites
    fi

    discover_tests

    if $RUN_PHASE1; then
        phase1_linux_tests
    fi

    if $RUN_PHASE15; then
        phase15_linux_qemu_tests
    fi

    if $RUN_STARRY; then
        phase2_starryos_tests
    fi

    print_summary
}

main "$@"
