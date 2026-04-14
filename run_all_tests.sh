#!/usr/bin/env bash
#
# run_all_tests.sh - Build and run Linux syscall compatibility tests
#                    on native/cross Linux and on StarryOS.
#
# Usage:
#   ./run_all_tests.sh [OPTIONS]
#
# Options:
#   --linux-only              Only run Phase 1 (Linux tests)
#   --linux-system            Also run Phase 1.5 (Linux guest reference in qemu-system)
#   --linux-system-only       Only run Phase 1.5 (Linux guest reference)
#   --linux-system-arch ARCH  Target Linux guest architecture (default: x86_64)
#   --linux-system-kernel P   Path to Linux guest kernel image
#   --linux-system-timeout S  Linux guest timeout in seconds (default: 90)
#   --starry-only             Only run Phase 2 (StarryOS tests)
#   --starry-arch ARCH        Target architecture for StarryOS (default: x86_64)
#   --tgoskits DIR            Path to tgoskits repo (default: auto-detect ../tgoskits)
#   --no-dep-check            Skip dependency checking
#   -h, --help            Show this help message
#
# Environment variables:
#   TGOSKITS_DIR          Path to tgoskits repo
#   STARRY_ARCH           Default StarryOS target arch
#   STARRY_TIMEOUT        StarryOS QEMU timeout in seconds (default: 90)
#   LINUX_SYSTEM_ARCH     Default Linux guest reference arch (default: x86_64)
#   LINUX_SYSTEM_KERNEL   Linux guest kernel image path
#   LINUX_SYSTEM_TIMEOUT  Linux guest timeout in seconds (default: 90)
#   LINUX_SYSTEM_MEMORY   Linux guest memory size (default: 512M)
#   LINUX_SYSTEM_ROOTFS_MB Linux guest ext4 image size in MiB (default: 128)
#   MUSL_CC               Override x86_64 musl compiler (e.g. x86_64-linux-musl-gcc)
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install musl-tools gcc-riscv64-linux-gnu gcc-aarch64-linux-gnu \
#                    qemu-user-static qemu-system-misc busybox-static rustc cargo e2fsprogs
#

set -euo pipefail

# ============================================================
#  Configuration
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TGOSKITS_DIR="${TGOSKITS_DIR:-}"
STARRY_ARCH="${STARRY_ARCH:-x86_64}"
STARRY_TIMEOUT="${STARRY_TIMEOUT:-90}"
LINUX_SYSTEM_ARCH="${LINUX_SYSTEM_ARCH:-x86_64}"
LINUX_SYSTEM_KERNEL="${LINUX_SYSTEM_KERNEL:-}"
LINUX_SYSTEM_TIMEOUT="${LINUX_SYSTEM_TIMEOUT:-90}"
LINUX_SYSTEM_MEMORY="${LINUX_SYSTEM_MEMORY:-512M}"
LINUX_SYSTEM_ROOTFS_MB="${LINUX_SYSTEM_ROOTFS_MB:-128}"
MUSL_CC="${MUSL_CC:-}"
HOST_ARCH="$(uname -m)"
[[ "$HOST_ARCH" == "amd64" ]] && HOST_ARCH="x86_64"

# Colors
C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[1;33m'
C_BLUE='\033[0;34m'; C_CYAN='\033[0;36m'; C_BOLD='\033[1m'; C_RESET='\033[0m'

# Logging
info()    { echo -e "  ${C_BLUE}INFO${C_RESET}  $*"; }
ok()      { echo -e "  ${C_GREEN} OK ${C_RESET}  $*"; }
warn()    { echo -e "  ${C_YELLOW}WARN${C_RESET}  $*"; }
err()     { echo -e "  ${C_RED} ERR${C_RESET}  $*"; }
section() { echo -e "\n${C_BOLD}${C_CYAN}──── $* ────${C_RESET}\n"; }
header()  { echo -e "\n${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}"; \
            echo -e "${C_BOLD}${C_BLUE}  $*${C_RESET}"; \
            echo -e "${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}\n"; }

# Test program definitions are discovered from test source files at runtime.
TEST_SOURCE_DIR=""
declare -a TEST_NAMES=()
declare -a TEST_SRCS=()
declare -a TEST_LDFLAGS=()
declare -a TEST_TIMEOUTS=()

# Cross-compiler command per architecture
declare -A CROSS_CC=(
    [x86_64]="gcc"
    [riscv64]="riscv64-linux-musl-gcc"
    [aarch64]="aarch64-linux-musl-gcc"
    [loongarch64]="loongarch64-linux-musl-gcc"
)

# QEMU user-mode emulator per architecture (for Linux cross-arch testing)
declare -A QEMU_USER=(
    [riscv64]="qemu-riscv64"
    [aarch64]="qemu-aarch64"
    [loongarch64]="qemu-loongarch64"
)

# StarryOS target triple per architecture
declare -A STARRY_TARGET=(
    [riscv64]="riscv64gc-unknown-none-elf"
    [aarch64]="aarch64-unknown-none-softfloat"
    [x86_64]="x86_64-unknown-none"
    [loongarch64]="loongarch64-unknown-none-softfloat"
)

# QEMU system emulator per architecture (for Linux guest reference testing)
declare -A QEMU_SYSTEM=(
    [x86_64]="qemu-system-x86_64"
    [riscv64]="qemu-system-riscv64"
    [aarch64]="qemu-system-aarch64"
    [loongarch64]="qemu-system-loongarch64"
)

# Directories
BUILD_DIR="${SCRIPT_DIR}/build"
LOG_DIR="${SCRIPT_DIR}/logs"

# Result tracking
declare -A LINUX_PASS LINUX_FAIL LINUX_SKIP LINUX_TEST_STATUS
declare -A LINUX_SYSTEM_TEST_STATUS LINUX_SYSTEM_TEST_RC
declare -A STARRY_TEST_STATUS STARRY_TEST_RC
LINUX_SYSTEM_TOTAL_PASS=0; LINUX_SYSTEM_TOTAL_FAIL=0; LINUX_SYSTEM_TOTAL_SKIP=0
STARRY_TOTAL_PASS=0; STARRY_TOTAL_FAIL=0; STARRY_TOTAL_SKIP=0
STARRY_COMPAT_MISMATCH=0
X86_64_MUSL_CC=""

# ============================================================
#  Argument Parsing
# ============================================================

RUN_LINUX=true
RUN_LINUX_SYSTEM=false
RUN_STARRY=true
SKIP_DEP_CHECK=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux-only)          RUN_STARRY=false; shift ;;
        --linux-system)        RUN_LINUX_SYSTEM=true; shift ;;
        --linux-system-only)   RUN_LINUX=false; RUN_STARRY=false; RUN_LINUX_SYSTEM=true; shift ;;
        --linux-system-arch)   LINUX_SYSTEM_ARCH="$2"; shift 2 ;;
        --linux-system-kernel) LINUX_SYSTEM_KERNEL="$2"; shift 2 ;;
        --linux-system-timeout) LINUX_SYSTEM_TIMEOUT="$2"; shift 2 ;;
        --starry-only)         RUN_LINUX=false; shift ;;
        --starry-arch)         STARRY_ARCH="$2"; shift 2 ;;
        --tgoskits)            TGOSKITS_DIR="$2"; shift 2 ;;
        --no-dep-check)        SKIP_DEP_CHECK=true; shift ;;
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
#  Helpers
# ============================================================

detect_x86_64_musl_cc() {
    local -a candidates=()

    if [[ -n "$MUSL_CC" ]]; then
        candidates+=("$MUSL_CC")
    fi
    candidates+=("x86_64-linux-musl-gcc" "musl-gcc")

    local candidate
    for candidate in "${candidates[@]}"; do
        if command -v "$candidate" &>/dev/null; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

detect_test_source_dir() {
    local candidate
    for candidate in "test_program" "tests"; do
        if [[ -d "${SCRIPT_DIR}/${candidate}" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

initialize_test_programs() {
    TEST_SOURCE_DIR="$(detect_test_source_dir || true)"
    if [[ -z "$TEST_SOURCE_DIR" ]]; then
        err "No test source directory found. Expected ${SCRIPT_DIR}/test_program or ${SCRIPT_DIR}/tests"
        exit 1
    fi

    TEST_NAMES=()
    TEST_SRCS=()
    TEST_LDFLAGS=()
    TEST_TIMEOUTS=()

    local -a src_paths=()
    local src_path

    while IFS= read -r src_path; do
        src_paths+=("${src_path#${SCRIPT_DIR}/}")
    done < <(find "${SCRIPT_DIR}/${TEST_SOURCE_DIR}" -maxdepth 1 -type f -name '*.c' | sort)

    if [[ ${#src_paths[@]} -eq 0 ]]; then
        err "No C test files found in ${SCRIPT_DIR}/${TEST_SOURCE_DIR}"
        exit 1
    fi

    for src_path in "${src_paths[@]}"; do
        local filename test_name
        filename="$(basename "$src_path")"
        test_name="${filename%.c}"

        TEST_NAMES+=("$test_name")
        TEST_SRCS+=("$src_path")
        TEST_LDFLAGS+=("")
        TEST_TIMEOUTS+=(30)
    done
}

compiler_for_arch() {
    local arch="$1"

    if [[ "$arch" == "x86_64" && -n "$X86_64_MUSL_CC" ]]; then
        printf '%s\n' "$X86_64_MUSL_CC"
    else
        printf '%s\n' "${CROSS_CC[$arch]:-}"
    fi
}

compiler_desc_for_arch() {
    local arch="$1"
    local cc
    cc="$(compiler_for_arch "$arch")"

    if [[ "$arch" == "x86_64" && -n "$X86_64_MUSL_CC" ]]; then
        printf '%s (musl)' "$cc"
    else
        printf '%s' "$cc"
    fi
}

need_x86_64_musl_cc() {
    [[ "$HOST_ARCH" == "x86_64" && "$RUN_LINUX" == true ]] || \
    [[ "$STARRY_ARCH" == "x86_64" && "$RUN_STARRY" == true ]] || \
    [[ "$LINUX_SYSTEM_ARCH" == "x86_64" && "$RUN_LINUX_SYSTEM" == true ]]
}

compile_one_test() {
    local arch="$1"
    local src_path="$2"
    local output_path="$3"
    local compile_log="$4"
    local ldflags="$5"

    local cc
    cc="$(compiler_for_arch "$arch")"
    if [[ -z "$cc" ]] || ! command -v "$cc" &>/dev/null; then
        return 127
    fi

    local -a extra_ldflags=()
    if [[ -n "$ldflags" ]]; then
        read -r -a extra_ldflags <<< "$ldflags"
    fi

    "$cc" -static -Wall -Wextra -O2 -o "$output_path" "$src_path" "${extra_ldflags[@]}" \
        >"$compile_log" 2>&1
}

escape_sed_replacement() {
    printf '%s' "$1" | sed 's/[\/&]/\\&/g'
}

write_starry_runner_script() {
    local runner_path="$1"
    local test_paths
    test_paths=$(printf '/bin/%s ' "${TEST_NAMES[@]}")

    cat >"$runner_path" <<RUNNER
#!/bin/sh

echo
echo "@@@ STARRY_TEST_SUITE_BEGIN @@@"
echo

TOTAL=0
PASSED=0
FAILED=0

for test_bin in ${test_paths}; do
    name=\${test_bin##*/}
    TOTAL=\$((TOTAL + 1))

    echo "@@@ STARRY_TEST_BEGIN \${name} @@@"
    "\$test_bin"
    rc=\$?

    if [ "\$rc" -eq 0 ]; then
        PASSED=\$((PASSED + 1))
        echo "@@@ STARRY_TEST_RESULT \${name} PASS rc=\${rc} @@@"
    else
        FAILED=\$((FAILED + 1))
        echo "@@@ STARRY_TEST_RESULT \${name} FAIL rc=\${rc} @@@"
    fi
    echo
done

echo "@@@ STARRY_TEST_SUMMARY total=\${TOTAL} passed=\${PASSED} failed=\${FAILED} @@@"
if [ "\$FAILED" -eq 0 ]; then
    echo "@@@ STARRY_TEST_SUITE PASS @@@"
    exit 0
else
    echo "@@@ STARRY_TEST_SUITE FAIL @@@"
    exit 1
fi
RUNNER

    chmod 0755 "$runner_path"
}

detect_linux_system_kernel() {
    if [[ -n "$LINUX_SYSTEM_KERNEL" ]]; then
        printf '%s\n' "$LINUX_SYSTEM_KERNEL"
        return 0
    fi

    local candidate
    for candidate in \
        "${SCRIPT_DIR}/linux-guest-assets/${LINUX_SYSTEM_ARCH}/bzImage" \
        "${SCRIPT_DIR}/linux-guest-assets/${LINUX_SYSTEM_ARCH}/vmlinuz" \
        "${SCRIPT_DIR}/linux-guest-assets/${LINUX_SYSTEM_ARCH}/Image"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

detect_linux_guest_busybox() {
    local candidate
    for candidate in /bin/busybox /usr/bin/busybox; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

validate_linux_system_kernel_arch() {
    local kernel_img="$1"
    local desc
    desc="$(file -b "$kernel_img" 2>/dev/null || true)"
    [[ -z "$desc" ]] && return 0

    case "$LINUX_SYSTEM_ARCH" in
        x86_64)
            if [[ "$desc" =~ RISC-V|AArch64|ARM64|LoongArch|ARM\  ]]; then
                err "Kernel image architecture doesn't match Linux guest arch ${LINUX_SYSTEM_ARCH}: ${desc}"
                return 1
            fi
            ;;
    esac

    return 0
}

write_linux_guest_init_source() {
    local init_src="$1"

    cat >"$init_src" <<'INIT_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s failed: %s\n", path, strerror(errno));
    }
}

static void try_mount(const char *src, const char *target, const char *fstype)
{
    if (mount(src, target, fstype, 0, "") == -1) {
        fprintf(stderr, "mount %s on %s failed: %s\n", fstype, target, strerror(errno));
    }
}

int main(void)
{
    ensure_dir("/proc");
    ensure_dir("/sys");
    ensure_dir("/dev");
    ensure_dir("/tmp");

    try_mount("proc", "/proc", "proc");
    try_mount("sysfs", "/sys", "sysfs");
    try_mount("devtmpfs", "/dev", "devtmpfs");

    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 127;
    }

    if (pid == 0) {
        char *const argv[] = { "/bin/run_syscall_tests.sh", NULL };
        char *const envp[] = { "PATH=/bin", "HOME=/", NULL };
        execve(argv[0], argv, envp);
        fprintf(stderr, "execve %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    int rc = 127;
    if (waitpid(pid, &status, 0) == pid) {
        if (WIFEXITED(status)) {
            rc = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            rc = 128 + WTERMSIG(status);
        }
    }

    printf("@@@ LINUX_SYSTEM_INIT_EXIT rc=%d @@@\n", rc);
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    reboot(RB_AUTOBOOT);
    return rc;
}
INIT_C
}

build_linux_guest_init_binary() {
    local output_path="$1"
    local compile_log="$2"
    local init_src="${output_path}.c"
    local cc

    cc="$(compiler_for_arch "$LINUX_SYSTEM_ARCH")"
    write_linux_guest_init_source "$init_src"

    "$cc" -static -no-pie -Wall -Wextra -O2 -o "$output_path" "$init_src" >"$compile_log" 2>&1
}

create_linux_guest_rootfs() {
    local rootfs_img="$1"
    local size_mb="$2"

    rm -f "$rootfs_img"
    truncate -s "${size_mb}M" "$rootfs_img"
    mkfs.ext4 -q -F -L linux-test-rootfs "$rootfs_img" >/dev/null
}

prepare_linux_guest_rootfs() {
    local rootfs_img="$1"
    local linux_build="$2"
    local busybox_path="$3"

    local runner_path="${linux_build}/run_syscall_tests.sh"
    local init_path="${linux_build}/init"
    local init_compile_log="${linux_build}/init_compile.log"
    local debugfs_cmds="${linux_build}/inject_rootfs.cmds"
    local inject_log="${linux_build}/inject_rootfs.log"

    write_starry_runner_script "$runner_path"
    if ! build_linux_guest_init_binary "$init_path" "$init_compile_log"; then
        cat "$init_compile_log"
        return 1
    fi

    create_linux_guest_rootfs "$rootfs_img" "$LINUX_SYSTEM_ROOTFS_MB"

    {
        echo "mkdir /bin"
        echo "mkdir /dev"
        echo "mkdir /proc"
        echo "mkdir /sys"
        echo "mkdir /tmp"
        echo "write ${busybox_path} /bin/busybox"
        echo "sif /bin/busybox mode 0100755"
        echo "symlink /bin/sh /bin/busybox"
        echo "write ${runner_path} /bin/run_syscall_tests.sh"
        echo "sif /bin/run_syscall_tests.sh mode 0100755"
        echo "write ${init_path} /init"
        echo "sif /init mode 0100755"
        echo "cd /dev"
        echo "mknod console c 5 1"
        echo "mknod null c 1 3"
        echo "cd /"
        local name
        for name in "${TEST_NAMES[@]}"; do
            echo "write ${linux_build}/${name} /bin/${name}"
            echo "sif /bin/${name} mode 0100755"
        done
    } >"$debugfs_cmds"

    inject_files_into_rootfs "$rootfs_img" "$debugfs_cmds" "$inject_log"
    e2fsck -fy "$rootfs_img" >"${linux_build}/rootfs_fsck.log" 2>&1 || true
}

run_linux_guest_qemu() {
    local kernel_img="$1"
    local rootfs_img="$2"
    local log_file="$3"

    local qemu_bin="${QEMU_SYSTEM[$LINUX_SYSTEM_ARCH]:-}"
    if [[ -z "$qemu_bin" ]]; then
        err "No qemu-system mapping defined for Linux guest arch: $LINUX_SYSTEM_ARCH"
        return 1
    fi

    run_logged_command "$log_file" \
        timeout "$LINUX_SYSTEM_TIMEOUT" \
        "$qemu_bin" \
        -nographic \
        -no-reboot \
        -m "$LINUX_SYSTEM_MEMORY" \
        -kernel "$kernel_img" \
        -append "console=ttyS0 root=/dev/vda rw init=/init panic=-1" \
        -drive "file=${rootfs_img},format=raw,if=virtio"
}

parse_marker_log() {
    local log_file="$1"
    local status_map_name="$2"
    local rc_map_name="$3"
    local total_pass_var="$4"
    local total_fail_var="$5"
    local total_skip_var="$6"
    local label="$7"

    local -n status_map="$status_map_name"
    local -n rc_map="$rc_map_name"
    local -n total_pass_ref="$total_pass_var"
    local -n total_fail_ref="$total_fail_var"
    local -n total_skip_ref="$total_skip_var"

    total_pass_ref=0
    total_fail_ref=0
    total_skip_ref=0

    local summary_line
    summary_line=$(grep -oE '@@@ STARRY_TEST_SUMMARY total=[0-9]+ passed=[0-9]+ failed=[0-9]+ @@@' \
        "$log_file" | tail -1 || true)

    if [[ -n "$summary_line" ]]; then
        total_pass_ref=$(echo "$summary_line" | grep -oE 'passed=[0-9]+' | cut -d= -f2)
        total_fail_ref=$(echo "$summary_line" | grep -oE 'failed=[0-9]+' | cut -d= -f2)
        local total
        total=$(echo "$summary_line" | grep -oE 'total=[0-9]+' | cut -d= -f2)
        total_skip_ref=$(( ${#TEST_NAMES[@]} - total ))
        info "${label}: ${summary_line}"
    else
        warn "${label}: could not parse summary marker"
    fi

    local name
    for name in "${TEST_NAMES[@]}"; do
        local line
        line=$(grep -E "@@@ STARRY_TEST_RESULT ${name} (PASS|FAIL) rc=[0-9]+ @@@" \
            "$log_file" | tail -1 || true)

        if [[ -z "$line" ]]; then
            status_map["$name"]="skip"
            rc_map["$name"]="-"
            if [[ -z "$summary_line" ]]; then
                ((total_skip_ref++))
            fi
            warn "${label}: $name not observed in serial log"
            continue
        fi

        local status rc
        status=$(echo "$line" | awk '{print $4}')
        rc=$(echo "$line" | awk '{print $5}' | cut -d= -f2)
        rc_map["$name"]="$rc"

        if [[ "$status" == "PASS" ]]; then
            status_map["$name"]="pass"
            ok "${label}: $name PASS"
        else
            status_map["$name"]="fail"
            err "${label}: $name FAIL (rc=$rc)"
        fi
    done
}

inject_files_into_rootfs() {
    local rootfs_img="$1"
    local debugfs_cmds="$2"
    local log_file="$3"

    if debugfs -w -f "$debugfs_cmds" "$rootfs_img" >"$log_file" 2>&1; then
        return 0
    fi

    cat "$log_file"
    return 1
}

prepare_starry_rootfs_copy() {
    local base_rootfs="$1"
    local work_rootfs="$2"
    local starry_build="$3"

    cp "$base_rootfs" "$work_rootfs"

    local runner_path="${starry_build}/run_syscall_tests.sh"
    local debugfs_cmds="${starry_build}/inject_rootfs.cmds"
    local inject_log="${starry_build}/inject_rootfs.log"

    write_starry_runner_script "$runner_path"

    {
        echo "write ${runner_path} /bin/run_syscall_tests.sh"
        echo "sif /bin/run_syscall_tests.sh mode 0100755"
        local name
        for name in "${TEST_NAMES[@]}"; do
            echo "write ${starry_build}/${name} /bin/${name}"
            echo "sif /bin/${name} mode 0100755"
        done
    } >"$debugfs_cmds"

    inject_files_into_rootfs "$work_rootfs" "$debugfs_cmds" "$inject_log"
}

generate_starry_qemu_config() {
    local template_path="$1"
    local output_path="$2"
    local disk_img="$3"

    local escaped_disk
    escaped_disk="$(escape_sed_replacement "$disk_img")"

    sed \
        -e "s|\"id=disk0,if=none,format=raw,file=.*\"|\"id=disk0,if=none,format=raw,file=${escaped_disk}\"|" \
        -e 's|^shell_init_cmd = .*|shell_init_cmd = "/bin/run_syscall_tests.sh"|' \
        -e 's|^success_regex = .*|success_regex = ["(?m)^@@@ STARRY_TEST_SUITE PASS @@@\\\\s*$"]|' \
        -e 's|^fail_regex = .*|fail_regex = ["(?m)^@@@ STARRY_TEST_SUITE FAIL @@@\\\\s*$", "(?i)\\\\bpanic(?:ked)?\\\\b"]|' \
        -e "s|^timeout = .*|timeout = ${STARRY_TIMEOUT}|" \
        "$template_path" >"$output_path"
}

summarize_log_tail() {
    local log_file="$1"
    if [[ -f "$log_file" ]]; then
        tail -20 "$log_file" || true
    fi
}

run_logged_command() {
    local log_file="$1"
    shift

    : >"$log_file"

    if [[ -t 1 ]]; then
        "$@" >"$log_file" 2>&1 &
        local cmd_pid=$!

        tail -n +1 -f "$log_file" &
        local tail_pid=$!

        local exit_code=0
        wait "$cmd_pid" || exit_code=$?

        kill "$tail_pid" >/dev/null 2>&1 || true
        wait "$tail_pid" 2>/dev/null || true
        return "$exit_code"
    fi

    "$@" >"$log_file" 2>&1
}

compare_starry_with_linux_reference() {
    local reference_kind=""
    local section_title=""

    if [[ -n "${LINUX_SYSTEM_TEST_STATUS["${TEST_NAMES[0]}"]:-}" ]]; then
        reference_kind="linux-system"
        section_title="StarryOS vs Linux/qemu-system Reference"
    elif [[ -n "${LINUX_TEST_STATUS["x86_64:${TEST_NAMES[0]}"]:-}" ]]; then
        reference_kind="linux-host"
        section_title="StarryOS vs Linux/x86_64 Reference"
    else
        return 0
    fi

    section "$section_title"

    local name
    for name in "${TEST_NAMES[@]}"; do
        local linux_status="unknown"
        local starry_status="${STARRY_TEST_STATUS[$name]:-unknown}"

        if [[ "$reference_kind" == "linux-system" ]]; then
            linux_status="${LINUX_SYSTEM_TEST_STATUS[$name]:-unknown}"
        else
            linux_status="${LINUX_TEST_STATUS["x86_64:${name}"]:-unknown}"
        fi

        if [[ "$linux_status" == "$starry_status" ]]; then
            ok "$name matches Linux reference ($linux_status)"
        else
            err "$name differs from Linux reference (linux=$linux_status, starry=$starry_status)"
            ((STARRY_COMPAT_MISMATCH++))
        fi
    done
}

# ============================================================
#  Prerequisites Check
# ============================================================

check_prerequisites() {
    header "Checking Prerequisites"

    local missing=()
    X86_64_MUSL_CC="$(detect_x86_64_musl_cc || true)"

    # Basic tools
    local cmd
    for cmd in gcc make timeout sed grep debugfs; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done

    if need_x86_64_musl_cc && [[ -z "$X86_64_MUSL_CC" ]]; then
        missing+=("x86_64 musl gcc (set MUSL_CC or install x86_64-linux-musl-gcc / musl-gcc)")
    fi

    # QEMU user-mode for cross-arch Linux testing
    local arch
    for arch in riscv64 aarch64; do
        if command -v "${QEMU_USER[$arch]}" &>/dev/null; then
            :
        elif command -v "${QEMU_USER[$arch]/-static/}" &>/dev/null; then
            QEMU_USER[$arch]="${QEMU_USER[$arch]/-static/}"
        else
            warn "QEMU user-mode for $arch not found — that arch will be skipped"
        fi
    done

    if $RUN_LINUX_SYSTEM; then
        local qemu_sys="${QEMU_SYSTEM[$LINUX_SYSTEM_ARCH]:-}"
        if [[ "$LINUX_SYSTEM_ARCH" != "x86_64" ]]; then
            err "Linux guest reference currently only supports x86_64"
            exit 1
        fi
        [[ -n "$qemu_sys" ]] && command -v "$qemu_sys" &>/dev/null || missing+=("${qemu_sys:-qemu-system-${LINUX_SYSTEM_ARCH}}")
        command -v mkfs.ext4 &>/dev/null || missing+=("mkfs.ext4")
        command -v e2fsck &>/dev/null || missing+=("e2fsck")

        local busybox_path kernel_path
        busybox_path="$(detect_linux_guest_busybox || true)"
        kernel_path="$(detect_linux_system_kernel || true)"

        if [[ -z "$busybox_path" ]]; then
            missing+=("busybox-static (busybox)")
        fi
        if [[ -z "$kernel_path" ]]; then
            missing+=("Linux guest kernel image (set --linux-system-kernel or LINUX_SYSTEM_KERNEL, or place one in linux-guest-assets/${LINUX_SYSTEM_ARCH}/)")
        else
            LINUX_SYSTEM_KERNEL="$kernel_path"
            validate_linux_system_kernel_arch "$LINUX_SYSTEM_KERNEL" || exit 1
        fi
    fi

    if $RUN_STARRY; then
        command -v cargo &>/dev/null || missing+=("cargo (Rust toolchain)")

        local qemu_sys="qemu-system-${STARRY_ARCH}"
        if ! command -v "$qemu_sys" &>/dev/null; then
            missing+=("$qemu_sys")
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
        echo ""
        info "Install on Ubuntu/Debian:"
        echo "  sudo apt install musl-tools gcc-riscv64-linux-gnu gcc-aarch64-linux-gnu \\"
        echo "       qemu-user-static qemu-system-misc busybox-static rustc cargo e2fsprogs"
        exit 1
    fi

    ok "Prerequisites OK"
    if [[ -n "$TGOSKITS_DIR" ]]; then
        info "tgoskits: $TGOSKITS_DIR"
    fi
    info "host arch: $HOST_ARCH"
    if [[ -n "$X86_64_MUSL_CC" ]]; then
        info "x86_64 musl compiler: $X86_64_MUSL_CC"
    fi
}

# ============================================================
#  Phase 1: Linux Multi-Architecture Tests
# ============================================================

phase1_linux_tests() {
    header "Phase 1: Linux Multi-Architecture Tests"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    local -a test_arches=()
    local arch
    for arch in x86_64 riscv64 aarch64; do
        local cc
        cc="$(compiler_for_arch "$arch")"
        if [[ "$arch" == "$HOST_ARCH" ]]; then
            if [[ -n "$cc" ]] && command -v "$cc" &>/dev/null; then
                test_arches+=("$arch")
            else
                warn "Skipping $arch: compiler $(compiler_desc_for_arch "$arch") not found"
            fi
        elif [[ -n "$cc" ]] && command -v "$cc" &>/dev/null; then
            local qemu="${QEMU_USER[$arch]:-}"
            if [[ -n "$qemu" ]] && command -v "$qemu" &>/dev/null; then
                test_arches+=("$arch")
            else
                warn "Skipping $arch: cross-compiler found but QEMU user-mode not found"
            fi
        else
            warn "Skipping $arch: compiler $(compiler_desc_for_arch "$arch") not installed"
        fi
    done

    if [[ ${#test_arches[@]} -eq 0 ]]; then
        err "No architectures available for Linux testing"
        return 1
    fi

    info "Will test on: ${test_arches[*]}"

    for arch in "${test_arches[@]}"; do
        section "Linux/$arch"
        local arch_build="${BUILD_DIR}/linux_${arch}"
        local arch_log="${LOG_DIR}/linux_${arch}"
        mkdir -p "$arch_build" "$arch_log"

        info "Compiler: $(compiler_desc_for_arch "$arch")"

        local compile_fail=0
        local i
        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            local src="${TEST_SRCS[$i]}"
            local ldflags="${TEST_LDFLAGS[$i]}"
            local src_path="${SCRIPT_DIR}/${src}"
            local compile_log="${arch_log}/${name}_compile.log"

            if [[ ! -f "$src_path" ]]; then
                err "Source not found: $src_path"
                LINUX_TEST_STATUS["$arch:$name"]="skip"
                compile_fail=1
                continue
            fi

            info "Compiling $name ($arch) ..."
            if compile_one_test "$arch" "$src_path" "${arch_build}/${name}" "$compile_log" "$ldflags"; then
                ok "Built $name"
            else
                err "Compile failed: $name ($arch)"
                cat "$compile_log"
                LINUX_TEST_STATUS["$arch:$name"]="fail"
                compile_fail=1
            fi
        done

        if [[ $compile_fail -ne 0 ]]; then
            warn "Some compilations failed for $arch, skipping execution"
            continue
        fi

        for i in "${!TEST_NAMES[@]}"; do
            local name="${TEST_NAMES[$i]}"
            local binary="${arch_build}/${name}"
            local test_timeout="${TEST_TIMEOUTS[$i]}"
            local log_file="${arch_log}/${name}_run.log"

            if [[ ! -f "$binary" ]]; then
                warn "SKIP $name ($arch): binary not found"
                LINUX_SKIP[$arch]=$((${LINUX_SKIP[$arch]:-0} + 1))
                LINUX_TEST_STATUS["$arch:$name"]="skip"
                continue
            fi

            info "Running $name on Linux/$arch (timeout: ${test_timeout}s) ..."

            local exit_code=0
            if [[ "$arch" == "$HOST_ARCH" ]]; then
                timeout "$test_timeout" "$binary" >"$log_file" 2>&1 || exit_code=$?
            else
                timeout "$test_timeout" "${QEMU_USER[$arch]}" "$binary" >"$log_file" 2>&1 || exit_code=$?
            fi

            if [[ $exit_code -eq 0 ]]; then
                ok "PASS  $name ($arch)"
                LINUX_PASS[$arch]=$((${LINUX_PASS[$arch]:-0} + 1))
                LINUX_TEST_STATUS["$arch:$name"]="pass"
            elif [[ $exit_code -eq 124 ]]; then
                err "TIMEOUT $name ($arch)"
                LINUX_FAIL[$arch]=$((${LINUX_FAIL[$arch]:-0} + 1))
                LINUX_TEST_STATUS["$arch:$name"]="fail"
                summarize_log_tail "$log_file"
            else
                err "FAIL  $name ($arch) — exit code $exit_code"
                LINUX_FAIL[$arch]=$((${LINUX_FAIL[$arch]:-0} + 1))
                LINUX_TEST_STATUS["$arch:$name"]="fail"
                summarize_log_tail "$log_file"
            fi
            echo ""
        done
    done
}

# ============================================================
#  Phase 1.5: Linux QEMU-System Guest Reference
# ============================================================

phase1_5_linux_system_reference() {
    header "Phase 1.5: Linux QEMU-System Guest Reference (${LINUX_SYSTEM_ARCH})"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    if [[ "$LINUX_SYSTEM_ARCH" != "x86_64" ]]; then
        err "Linux guest reference currently only supports x86_64"
        return 1
    fi

    local busybox_path
    busybox_path="$(detect_linux_guest_busybox || true)"
    if [[ -z "$busybox_path" ]]; then
        err "busybox-static not found"
        return 1
    fi

    LINUX_SYSTEM_KERNEL="$(detect_linux_system_kernel || true)"
    if [[ -z "$LINUX_SYSTEM_KERNEL" || ! -f "$LINUX_SYSTEM_KERNEL" ]]; then
        err "Linux guest kernel not found. Use --linux-system-kernel or LINUX_SYSTEM_KERNEL."
        return 1
    fi
    validate_linux_system_kernel_arch "$LINUX_SYSTEM_KERNEL" || return 1

    local arch_build="${BUILD_DIR}/linux_system_${LINUX_SYSTEM_ARCH}"
    local arch_log="${LOG_DIR}/linux_system_${LINUX_SYSTEM_ARCH}"
    mkdir -p "$arch_build" "$arch_log"

    section "Linux guest build (${LINUX_SYSTEM_ARCH})"
    info "Compiler: $(compiler_desc_for_arch "$LINUX_SYSTEM_ARCH")"
    info "Kernel: ${LINUX_SYSTEM_KERNEL}"
    info "Busybox: ${busybox_path}"

    local i
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local src="${TEST_SRCS[$i]}"
        local ldflags="${TEST_LDFLAGS[$i]}"
        local src_path="${SCRIPT_DIR}/${src}"
        local compile_log="${arch_log}/${name}_compile.log"

        info "Compiling $name (${LINUX_SYSTEM_ARCH}, guest) ..."
        if compile_one_test "$LINUX_SYSTEM_ARCH" "$src_path" "${arch_build}/${name}" "$compile_log" "$ldflags"; then
            ok "Built $name for Linux guest"
        else
            err "Compile failed: $name"
            cat "$compile_log"
            return 1
        fi
    done

    section "Linux guest rootfs"
    local rootfs_img="${arch_build}/rootfs-${LINUX_SYSTEM_ARCH}-linux-guest.img"
    if prepare_linux_guest_rootfs "$rootfs_img" "$arch_build" "$busybox_path"; then
        ok "Prepared Linux guest ext4 rootfs"
    else
        err "Failed to prepare Linux guest rootfs"
        return 1
    fi

    section "Linux guest run"
    local qemu_log="${arch_log}/qemu_output.log"
    local qemu_exit_code=0

    info "Launching Linux guest in qemu-system (timeout: ${LINUX_SYSTEM_TIMEOUT}s) ..."
    set +e
    run_linux_guest_qemu "$LINUX_SYSTEM_KERNEL" "$rootfs_img" "$qemu_log"
    qemu_exit_code=$?
    set -e

    if [[ $qemu_exit_code -eq 124 ]]; then
        warn "Linux guest QEMU timed out after ${LINUX_SYSTEM_TIMEOUT}s"
    elif [[ $qemu_exit_code -ne 0 ]]; then
        warn "Linux guest QEMU exited with code $qemu_exit_code"
    fi

    if [[ ! -s "$qemu_log" ]]; then
        err "No Linux guest serial output captured"
        return 1
    fi

    echo ""
    echo "--- Linux guest serial output (markers) ---"
    grep '@@@ STARRY_TEST_' "$qemu_log" || true
    echo "--- End of markers ---"
    echo ""

    parse_marker_log \
        "$qemu_log" \
        LINUX_SYSTEM_TEST_STATUS \
        LINUX_SYSTEM_TEST_RC \
        LINUX_SYSTEM_TOTAL_PASS \
        LINUX_SYSTEM_TOTAL_FAIL \
        LINUX_SYSTEM_TOTAL_SKIP \
        "Linux guest"

    info "Linux guest log: $qemu_log"
    info "Linux guest rootfs: $rootfs_img"
}

# ============================================================
#  Phase 2: StarryOS Tests
# ============================================================

phase2_starryos_tests() {
    header "Phase 2: StarryOS Tests ($STARRY_ARCH)"
    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    local starry_build="${BUILD_DIR}/starry_${STARRY_ARCH}"
    local starry_log="${LOG_DIR}/starry_${STARRY_ARCH}"
    mkdir -p "$starry_build" "$starry_log"

    local cc
    cc="$(compiler_for_arch "$STARRY_ARCH")"
    if [[ -z "$cc" ]] || ! command -v "$cc" &>/dev/null; then
        err "Compiler for $STARRY_ARCH not found: $(compiler_desc_for_arch "$STARRY_ARCH")"
        return 1
    fi

    local target_triple="${STARRY_TARGET[$STARRY_ARCH]:-}"
    if [[ -z "$target_triple" ]]; then
        err "Unsupported StarryOS architecture: $STARRY_ARCH"
        return 1
    fi

    # ----------------------------------------------------------
    #  Step 1: Cross-compile test programs as static binaries
    # ----------------------------------------------------------
    section "Step 1/4: Cross-compile for StarryOS/$STARRY_ARCH"
    info "Compiler: $(compiler_desc_for_arch "$STARRY_ARCH")"

    local i
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        local src="${TEST_SRCS[$i]}"
        local ldflags="${TEST_LDFLAGS[$i]}"
        local src_path="${SCRIPT_DIR}/${src}"
        local compile_log="${starry_log}/${name}_compile.log"

        info "Compiling $name ($STARRY_ARCH, static) ..."
        if compile_one_test "$STARRY_ARCH" "$src_path" "${starry_build}/${name}" "$compile_log" "$ldflags"; then
            ok "Built $name for StarryOS"
        else
            err "Compile failed: $name"
            cat "$compile_log"
            return 1
        fi
    done

    # ----------------------------------------------------------
    #  Step 2: Prepare isolated rootfs copy and inject binaries
    # ----------------------------------------------------------
    section "Step 2/4: Prepare isolated rootfs"

    info "Ensuring StarryOS rootfs is available ..."
    (
        cd "$TGOSKITS_DIR"
        cargo starry rootfs --arch "$STARRY_ARCH"
    )
    ok "Rootfs ready"

    local rootfs="${TGOSKITS_DIR}/target/${target_triple}/rootfs-${STARRY_ARCH}.img"
    if [[ ! -f "$rootfs" ]]; then
        err "Rootfs not found: $rootfs"
        return 1
    fi

    local work_rootfs="${starry_build}/rootfs-${STARRY_ARCH}-tests.img"
    info "Creating isolated rootfs copy: $work_rootfs"
    if prepare_starry_rootfs_copy "$rootfs" "$work_rootfs" "$starry_build"; then
        ok "Injected runner and test binaries into isolated rootfs"
    else
        err "Failed to inject files into rootfs"
        return 1
    fi

    # ----------------------------------------------------------
    #  Step 3: Generate dedicated QEMU config for this run
    # ----------------------------------------------------------
    section "Step 3/4: Generate dedicated QEMU config"

    local qemu_template="${TGOSKITS_DIR}/test-suit/starryos/qemu-${STARRY_ARCH}.toml"
    local qemu_config="${starry_build}/qemu-${STARRY_ARCH}-syscall-tests.toml"
    if [[ ! -f "$qemu_template" ]]; then
        err "QEMU template not found: $qemu_template"
        return 1
    fi

    generate_starry_qemu_config "$qemu_template" "$qemu_config" "$work_rootfs"
    ok "Generated test QEMU config: $qemu_config"

    # ----------------------------------------------------------
    #  Step 4: Build and run StarryOS in QEMU
    # ----------------------------------------------------------
    section "Step 4/4: Run tests on StarryOS ($STARRY_ARCH)"

    local starry_output="${starry_log}/qemu_output.log"
    local qemu_exit_code=0

    info "Launching StarryOS in QEMU (timeout: ${STARRY_TIMEOUT}s) ..."
    info "Output will be captured to: $starry_output"

    set +e
    run_logged_command "$starry_output" bash -lc "
        cd '$TGOSKITS_DIR'
        timeout '$STARRY_TIMEOUT' cargo starry qemu --arch '$STARRY_ARCH' --qemu-config '$qemu_config'
    "
    qemu_exit_code=$?
    set -e

    section "StarryOS Test Results"

    if [[ $qemu_exit_code -eq 124 ]]; then
        warn "QEMU timed out after ${STARRY_TIMEOUT}s"
    elif [[ $qemu_exit_code -ne 0 ]]; then
        warn "cargo starry qemu exited with code $qemu_exit_code"
    fi

    if [[ ! -s "$starry_output" ]]; then
        err "No QEMU output captured"
        return 1
    fi

    echo ""
    echo "--- StarryOS serial output (markers) ---"
    grep '@@@ STARRY_TEST_' "$starry_output" || true
    echo "--- End of markers ---"
    echo ""

    local summary_line
    summary_line=$(grep -oE '@@@ STARRY_TEST_SUMMARY total=[0-9]+ passed=[0-9]+ failed=[0-9]+ @@@' \
        "$starry_output" | tail -1 || true)

    if [[ -n "$summary_line" ]]; then
        local total passed failed
        total=$(echo "$summary_line" | grep -oE 'total=[0-9]+' | cut -d= -f2)
        passed=$(echo "$summary_line" | grep -oE 'passed=[0-9]+' | cut -d= -f2)
        failed=$(echo "$summary_line" | grep -oE 'failed=[0-9]+' | cut -d= -f2)
        STARRY_TOTAL_PASS="$passed"
        STARRY_TOTAL_FAIL="$failed"
        STARRY_TOTAL_SKIP=$(( ${#TEST_NAMES[@]} - total ))
        info "$summary_line"
    else
        warn "Could not parse StarryOS summary marker"
    fi

    local name
    for name in "${TEST_NAMES[@]}"; do
        local line
        line=$(grep -E "@@@ STARRY_TEST_RESULT ${name} (PASS|FAIL) rc=[0-9]+ @@@" \
            "$starry_output" | tail -1 || true)

        if [[ -z "$line" ]]; then
            STARRY_TEST_STATUS["$name"]="skip"
            STARRY_TEST_RC["$name"]="-"
            if [[ -z "$summary_line" ]]; then
                ((STARRY_TOTAL_SKIP++))
            fi
            warn "StarryOS: $name not observed in serial log"
            continue
        fi

        local status rc
        status=$(echo "$line" | awk '{print $4}')
        rc=$(echo "$line" | awk '{print $5}' | cut -d= -f2)
        STARRY_TEST_RC["$name"]="$rc"

        if [[ "$status" == "PASS" ]]; then
            STARRY_TEST_STATUS["$name"]="pass"
            ok "StarryOS: $name PASS"
        else
            STARRY_TEST_STATUS["$name"]="fail"
            err "StarryOS: $name FAIL (rc=$rc)"
        fi
    done

    compare_starry_with_linux_reference

    info "Full QEMU log: $starry_output"
    info "Isolated rootfs: $work_rootfs"
    info "QEMU config: $qemu_config"
}

# ============================================================
#  Summary
# ============================================================

print_summary() {
    header "Test Summary"

    if $RUN_LINUX; then
        echo -e "${C_BOLD}Phase 1 — Linux Multi-Arch Tests:${C_RESET}"
        local arch
        for arch in x86_64 riscv64 aarch64; do
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

    if $RUN_LINUX_SYSTEM; then
        echo -e "${C_BOLD}Phase 1.5 — Linux QEMU-System Reference (${LINUX_SYSTEM_ARCH}):${C_RESET}"
        printf "  %-12s " "${LINUX_SYSTEM_ARCH}:"
        if [[ $LINUX_SYSTEM_TOTAL_FAIL -eq 0 && $LINUX_SYSTEM_TOTAL_PASS -gt 0 ]]; then
            echo -e "${C_GREEN}${LINUX_SYSTEM_TOTAL_PASS} passed${C_RESET}, ${LINUX_SYSTEM_TOTAL_SKIP} skipped"
        elif [[ $LINUX_SYSTEM_TOTAL_FAIL -gt 0 ]]; then
            echo -e "${C_RED}${LINUX_SYSTEM_TOTAL_PASS} passed, ${LINUX_SYSTEM_TOTAL_FAIL} FAILED${C_RESET}, ${LINUX_SYSTEM_TOTAL_SKIP} skipped"
        else
            echo -e "${LINUX_SYSTEM_TOTAL_PASS} passed, ${LINUX_SYSTEM_TOTAL_FAIL} failed, ${LINUX_SYSTEM_TOTAL_SKIP} skipped"
        fi
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
        if [[ $STARRY_COMPAT_MISMATCH -gt 0 ]]; then
            if [[ -n "${LINUX_SYSTEM_TEST_STATUS["${TEST_NAMES[0]}"]:-}" ]]; then
                echo -e "  Compatibility: ${C_RED}${STARRY_COMPAT_MISMATCH} mismatch(es) vs Linux/qemu-system${C_RESET}"
            else
                echo -e "  Compatibility: ${C_RED}${STARRY_COMPAT_MISMATCH} mismatch(es) vs Linux/x86_64${C_RESET}"
            fi
        elif [[ -n "${LINUX_SYSTEM_TEST_STATUS["${TEST_NAMES[0]}"]:-}" ]]; then
            echo -e "  Compatibility: ${C_GREEN}matched Linux/qemu-system reference${C_RESET}"
        elif $RUN_LINUX; then
            echo -e "  Compatibility: ${C_GREEN}matched Linux/x86_64 reference${C_RESET}"
        fi
        echo ""
    fi

    echo -e "Logs: ${LOG_DIR}/"
    echo -e "Build artifacts: ${BUILD_DIR}/"
    echo ""

    local total_fail=0
    if $RUN_LINUX; then
        local arch
        for arch in x86_64 riscv64 aarch64; do
            total_fail=$(( total_fail + ${LINUX_FAIL[$arch]:-0} ))
        done
    fi

    if $RUN_LINUX_SYSTEM; then
        total_fail=$(( total_fail + LINUX_SYSTEM_TOTAL_FAIL ))
    fi

    if $RUN_STARRY; then
        total_fail=$(( total_fail + STARRY_TOTAL_FAIL + STARRY_COMPAT_MISMATCH ))
    fi

    if [[ $total_fail -eq 0 ]]; then
        ok "All tests PASSED"
        return 0
    else
        err "$total_fail issue(s) detected"
        return 1
    fi
}

# ============================================================
#  Main
# ============================================================

initialize_test_programs

main() {
    echo -e "${C_BOLD}╔══════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_BOLD}║       Syscall Test Runner — Linux & StarryOS    ║${C_RESET}"
    echo -e "${C_BOLD}╚══════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo "  Test repo:      $SCRIPT_DIR"
    echo "  StarryOS:       ${TGOSKITS_DIR:-<not set>}"
    echo "  Host arch:      $HOST_ARCH"
    echo "  Linux guest:    ${LINUX_SYSTEM_ARCH} (enabled=${RUN_LINUX_SYSTEM})"
    echo "  StarryOS arch:  $STARRY_ARCH"
    echo "  Test source:    ${SCRIPT_DIR}/${TEST_SOURCE_DIR} (${#TEST_NAMES[@]} tests)"
    echo ""

    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    if ! $SKIP_DEP_CHECK; then
        check_prerequisites
    else
        X86_64_MUSL_CC="$(detect_x86_64_musl_cc || true)"
    fi

    if $RUN_LINUX; then
        phase1_linux_tests
    fi

    if $RUN_LINUX_SYSTEM; then
        phase1_5_linux_system_reference
    fi

    if $RUN_STARRY; then
        phase2_starryos_tests
    fi

    print_summary
}

main "$@"
