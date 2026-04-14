#!/usr/bin/env bash
#
# run_all_tests.sh - Compile and run C syscall test programs on Linux (multi-arch)
#                    and on StarryOS
#
# Usage:
#   ./run_all_tests.sh [OPTIONS]
#
# Options:
#   --linux-only          Only run Phase 1 (Linux multi-arch tests)
#   --starry-only         Only run Phase 2 (StarryOS tests)
#   --starry-arch ARCH    Target architecture for StarryOS (default: riscv64)
#   --tgoskits DIR        Path to tgoskits repo (default: auto-detect ../tgoskits)
#   --no-dep-check        Skip dependency checking
#   -h, --help            Show this help message
#
# Environment variables:
#   TGOSKITS_DIR       Path to tgoskits repo
#   STARRY_ARCH        Default StarryOS target arch
#   STARRY_TIMEOUT     Per-test QEMU timeout in seconds (default: 60)
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install gcc-riscv64-linux-gnu gcc-aarch64-linux-gnu \
#                    qemu-user-static qemu-system-misc rustc cargo \
#                    sudo e2fsprogs
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

# Logging
info()    { echo -e "  ${C_BLUE}INFO${C_RESET}  $*"; }
ok()      { echo -e "  ${C_GREEN} OK ${C_RESET}  $*"; }
warn()    { echo -e "  ${C_YELLOW}WARN${C_RESET}  $*"; }
err()     { echo -e "  ${C_RED} ERR${C_RESET}  $*"; }
section() { echo -e "\n${C_BOLD}${C_CYAN}──── $* ────${C_RESET}\n"; }
header()  { echo -e "\n${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}"; \
            echo -e "${C_BOLD}${C_BLUE}  $*${C_RESET}"; \
            echo -e "${C_BOLD}${C_BLUE}══════════════════════════════════════════════════${C_RESET}\n"; }

# Test program definitions.
# Keep this list aligned with docs/syscall_priority.md and tests/.
declare -a TEST_NAMES=(
    "test_basic_io"
    "test_openat"
    "test_stat"
    "test_pipe2"
    "test_fork_v2"
    "test_dup_v2"
    "test_mmap"
    "test_signal"
    "test_brk"
    "test_accept4"
)
declare -a TEST_SRCS=(
    "tests/test_basic_io.c"
    "tests/test_openat.c"
    "tests/test_stat.c"
    "tests/test_pipe2.c"
    "tests/test_fork_v2.c"
    "tests/test_dup_v2.c"
    "tests/test_mmap.c"
    "tests/test_signal.c"
    "tests/test_brk.c"
    "tests/test_accept4.c"
)
declare -a TEST_LDFLAGS=("" "" "" "" "" "" "" "" "" "")
declare -a TEST_TIMEOUTS=(30 30 30 30 60 30 30 30 30 30)  # Per-test timeout in seconds

# Cross-compiler command per architecture
declare -A CROSS_CC=(
    [x86_64]="gcc"
    [riscv64]="riscv64-linux-gnu-gcc"
    [aarch64]="aarch64-linux-gnu-gcc"
    [loongarch64]="loongarch64-linux-gnu-gcc"
)

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

RUN_LINUX=true
RUN_STARRY=true
SKIP_DEP_CHECK=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux-only)    RUN_STARRY=false; shift ;;
        --starry-only)   RUN_LINUX=false; shift ;;
        --starry-arch)   STARRY_ARCH="$2"; shift 2 ;;
        --tgoskits)      TGOSKITS_DIR="$2"; shift 2 ;;
        --no-dep-check)  SKIP_DEP_CHECK=true; shift ;;
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

    # QEMU user-mode for cross-arch Linux testing
    for arch in riscv64 aarch64; do
        if command -v "${QEMU_USER[$arch]}" &>/dev/null; then
            : # available
        elif command -v "${QEMU_USER[$arch]/-static/}" &>/dev/null; then
            # Some distros install without -static suffix
            QEMU_USER[$arch]="${QEMU_USER[$arch]/-static/}"
        else
            warn "QEMU user-mode for $arch not found — that arch will be skipped"
        fi
    done

    if $RUN_STARRY; then
        command -v cargo &>/dev/null || missing+=("cargo (Rust toolchain)")
        command -v sudo &>/dev/null   || missing+=("sudo")

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
        echo "  sudo apt install gcc-riscv64-linux-gnu gcc-aarch64-linux-gnu \\"
        echo "       qemu-user-static qemu-system-misc qemu-system-arm \\"
        echo "       rustc cargo sudo e2fsprogs"
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
    for arch in x86_64 riscv64 aarch64; do
        local cc="${CROSS_CC[$arch]}"
        if [[ "$arch" == "$HOST_ARCH" ]]; then
            test_arches+=("$arch")
        elif command -v "$cc" &>/dev/null; then
            # Also need QEMU user-mode for non-native arches
            local qemu="${QEMU_USER[$arch]:-}"
            if [[ -n "$qemu" ]] && command -v "$qemu" &>/dev/null; then
                test_arches+=("$arch")
            else
                warn "Skipping $arch: cross-compiler found but QEMU user-mode not found"
            fi
        else
            warn "Skipping $arch: $cc not installed"
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
            local src="${TEST_SRCS[$i]}"
            local ldflags="${TEST_LDFLAGS[$i]}"
            local src_path="${SCRIPT_DIR}/${src}"

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
        local src="${TEST_SRCS[$i]}"
        local ldflags="${TEST_LDFLAGS[$i]}"
        local src_path="${SCRIPT_DIR}/${src}"

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

    # Mount and inject
    info "Mounting rootfs and injecting test binaries ..."
    local mountpoint="/tmp/starry_rootfs_${STARRY_ARCH}_$$"
    mkdir -p "$mountpoint"
    _CLEANUP_MOUNTS+=("$mountpoint")

    sudo mount -o loop "$work_rootfs" "$mountpoint"

    # Copy test binaries
    for i in "${!TEST_NAMES[@]}"; do
        local name="${TEST_NAMES[$i]}"
        sudo cp "${starry_build}/${name}" "${mountpoint}/bin/${name}"
        sudo chmod 0755 "${mountpoint}/bin/${name}"
    done
    ok "Test binaries copied to /bin/"

    # Create the test runner script on rootfs
    local runner_tests
    runner_tests=$(printf '/bin/%s ' "${TEST_NAMES[@]}")

    sudo tee "${mountpoint}/bin/run_syscall_tests.sh" >/dev/null <<RUNNER
#!/bin/sh
# ──────────────────────────────────────────────
#  Automated Syscall Test Runner for StarryOS
# ──────────────────────────────────────────────
echo ""
echo "========================================="
echo "  StarryOS Syscall Test Suite"
echo "========================================="
echo ""

TOTAL=0
PASSED=0
FAILED=0

for test_bin in ${runner_tests}; do
    if [ -x "$test_bin" ]; then
        echo "--- Running $(basename $test_bin) ---"
        TOTAL=$((TOTAL + 1))
        if $test_bin; then
            PASSED=$((PASSED + 1))
            echo "--- $(basename $test_bin): PASSED ---"
        else
            FAILED=$((FAILED + 1))
            echo "--- $(basename $test_bin): FAILED (exit=$?) ---"
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
    sudo chmod 0755 "${mountpoint}/bin/run_syscall_tests.sh"
    ok "Test runner script installed to /bin/run_syscall_tests.sh"

    # Ensure /tmp exists (some tests create temp files there)
    sudo mkdir -p "${mountpoint}/tmp"
    sudo chmod 1777 "${mountpoint}/tmp"

    # Create /root/.profile so the login shell auto-runs tests and exits.
    # The init.sh does: cd ~ && sh --login
    # BusyBox ash --login sources $HOME/.profile
    sudo tee "${mountpoint}/root/.profile" >/dev/null << 'PROFILE'
#!/bin/sh
echo ""
echo "[auto-test] Running syscall tests from .profile ..."
/bin/run_syscall_tests.sh
echo ""
echo "[auto-test] Tests complete. Exiting shell."
exit 0
PROFILE
    sudo chmod 0755 "${mountpoint}/root/.profile"
    ok "Auto-test .profile installed"

    # Unmount
    sudo umount "$mountpoint"
    rmdir "$mountpoint"
    # Remove from cleanup list since we already cleaned up
    _CLEANUP_MOUNTS=()
    ok "Rootfs prepared and unmounted"

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
    info "Output will be captured to: $starry_output"

    # Run StarryOS in QEMU with timeout
    # Note: the kernel starts /bin/sh -c "init.sh" which runs sh --login,
    # which sources .profile, which runs our tests and exits.
    # After init exits, the kernel halts and QEMU should terminate.
    set +e
    ( cd "$TGOSKITS_DIR" && timeout "$STARRY_TIMEOUT" cargo starry qemu --arch "$STARRY_ARCH" ) \
        > "$starry_output" 2>&1
    qemu_exit_code=$?
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

    if $RUN_LINUX; then
        echo -e "${C_BOLD}Phase 1 — Linux Multi-Arch Tests:${C_RESET}"
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
    if $RUN_LINUX; then
        for arch in x86_64 riscv64 aarch64; do
            total_fail=$(( total_fail + ${LINUX_FAIL[$arch]:-0} ))
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
    echo "  Test repo:   $SCRIPT_DIR"
    echo "  StarryOS:    ${TGOSKITS_DIR:-<not set>}"
    echo "  Host arch:   $HOST_ARCH"
    echo "  StarryOS arch: $STARRY_ARCH"
    echo ""

    mkdir -p "$BUILD_DIR" "$LOG_DIR"

    if ! $SKIP_DEP_CHECK; then
        check_prerequisites
    fi

    if $RUN_LINUX; then
        phase1_linux_tests
    fi

    if $RUN_STARRY; then
        phase2_starryos_tests
    fi

    print_summary
}

main "$@"
