#!/bin/bash
#
# ailink BSP Build & Flash Script (RT-Thread / STM32F446ZE)
#
# Usage: ./run.sh [command] [options]
#

set -o pipefail

# ==============================================================================
# Configuration
# ==============================================================================
readonly RTT_ROOT="${RTT_ROOT:-$HOME/SDK/rt-thread}"
readonly RTT_EXEC_PATH="${RTT_EXEC_PATH:-$HOME/toolchain/arm-eabi-toolchain/bin}"
readonly VENV_DIR=".venv"
readonly BUILD_DIR="./build"
readonly PROJECT_NAME="${AILINK_PROJECT_NAME:-ailink}"

# OpenOCD: ST-Link probe by default (Nucleo onboard), override via env,
# e.g. OPENOCD_INTERFACE=jlink or OPENOCD_INTERFACE=cmsis-dap.
readonly OPENOCD_INTERFACE="${OPENOCD_INTERFACE:-stlink}"
readonly OPENOCD_TARGET="${OPENOCD_TARGET:-stm32f4x}"
readonly FLASH_ADDR="0x08000000"

# Colors & Formatting
readonly R='\033[0;31m'   # Red
readonly G='\033[0;32m'   # Green
readonly B='\033[0;34m'   # Blue
readonly NC='\033[0m'     # No Color

VERBOSE=0

# ==============================================================================
# Helper Functions
# ==============================================================================

log_info() { echo -e "${B}[INFO]${NC} $1"; }
log_success() { echo -e "${G}[SUCCESS]${NC} $1"; }
log_error() { echo -e "${R}[ERROR]${NC} $1" >&2; }
die() { log_error "$1"; exit 1; }

# rtconfig.py owns the full environment contract; this only fails fast with
# clearer messages before SCons is even invoked.
check_toolchain() {
    [[ -x "$RTT_EXEC_PATH/arm-none-eabi-gcc" ]] || die "Toolchain not found at: $RTT_EXEC_PATH"
    [[ -f "$RTT_ROOT/tools/building.py" ]] || die "RT-Thread source not found at: $RTT_ROOT"
    export RTT_ROOT RTT_EXEC_PATH
    export PROJECT_NAME
    ensure_venv
}

# Python build deps (scons/kconfiglib) are version-pinned in a project venv
# so builds never depend on the system scons version.
ensure_venv() {
    local stamp="$VENV_DIR/.requirements.stamp"

    if [[ ! -x "$VENV_DIR/bin/python" ]]; then
        log_info "Creating Python venv ($VENV_DIR)..."
        python3 -m venv "$VENV_DIR" || die "venv creation failed (is python3-venv installed?)."
    fi

    if ! cmp -s requirements.txt "$stamp"; then
        log_info "Installing pinned Python deps (requirements.txt)..."
        "$VENV_DIR/bin/pip" install --quiet -r requirements.txt \
            || die "Failed to install Python deps into $VENV_DIR."
        cp requirements.txt "$stamp"
    fi

    # Prepend venv so scons / python resolve to the pinned environment.
    export VIRTUAL_ENV="$PWD/$VENV_DIR"
    export PATH="$VIRTUAL_ENV/bin:$PATH"
}

parse_mode() {
    local mode="Release"
    for arg in "$@"; do
        case "$arg" in
            debug|--debug|-d)   mode="Debug" ;;
            release|--release|-r) mode="Release" ;;
        esac
    done
    echo "$mode"
}

update_compile_commands() {
    if [[ -f "compile_commands.json" ]] && grep -q '"output"' "compile_commands.json"; then
        mkdir -p .vscode
        mv compile_commands.json .vscode/
        log_info "Updated compile_commands.json"
    else
        [[ -f "compile_commands.json" ]] && rm -f compile_commands.json
    fi
}

is_mode_arg() {
    case "$1" in
        debug|--debug|-d|release|--release|-r) return 0 ;;
        *) return 1 ;;
    esac
}

validate_mode_args() {
    local cmd="$1"
    shift
    local arg
    for arg in "$@"; do
        is_mode_arg "$arg" || die "Unsupported argument for $cmd: $arg"
    done
}

validate_no_args() {
    local cmd="$1"
    shift
    [[ $# -eq 0 ]] || die "$cmd does not accept arguments: $*"
}

# ==============================================================================
# Commands
# ==============================================================================

get_app_bin() {
    echo "$BUILD_DIR/${PROJECT_NAME}.bin"
}

run_scons_build() {
    local mode="$1"

    export BUILD_MODE="$mode"
    rm -f "$BUILD_DIR/${PROJECT_NAME}.map"

    # Run build with bear to generate compile commands
    # Use -Q for quiet mode (suppress "Reading/Building" messages)
    local scons_build_args=(-j"$(nproc)" -Q)
    if [[ "$VERBOSE" -eq 0 ]]; then
        scons_build_args+=("--silent")
    fi

    if ! bear --output compile_commands.json -- scons "${scons_build_args[@]}"; then
        die "Build failed."
    fi

    update_compile_commands
}

cmd_build() {
    check_toolchain

    local mode
    mode=$(parse_mode "$@")

    log_info "Building ${PROJECT_NAME} (Mode: $mode)..."

    run_scons_build "$mode"
    [[ -f "$(get_app_bin)" ]] || die "Firmware binary not found: $(get_app_bin)"

    log_success "Build complete: $(get_app_bin)"
}

cmd_clean() {
    log_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR" .sconsign.dblite || die "Failed to remove build directory: $BUILD_DIR"

    log_success "Clean complete."
}

cmd_flash() {
    local bin_path
    bin_path=$(get_app_bin)
    [[ -f "$bin_path" ]] || die "Firmware not found: $bin_path. Build first."

    command -v openocd > /dev/null || die "openocd not found in PATH."

    log_info "Flashing $bin_path to $FLASH_ADDR (${OPENOCD_INTERFACE}/${OPENOCD_TARGET})..."

    local openocd_args=(
        -f "interface/${OPENOCD_INTERFACE}.cfg"
        -f "target/${OPENOCD_TARGET}.cfg"
        -c "program $bin_path $FLASH_ADDR verify reset exit"
    )

    openocd "${openocd_args[@]}" || die "Flashing failed (OpenOCD error)."

    log_success "Flash complete."
}

show_help() {
    cat <<EOF
Usage: $0 <command> [options]

Commands:
  build [mode]            Build firmware (${BUILD_DIR#./}/${PROJECT_NAME}.bin)
  rebuild [mode]          Clean and build firmware
  rebuild-flash [mode]    Clean, build, and flash firmware
  clean                   Clean build artifacts
  flash                   Flash firmware to $FLASH_ADDR via OpenOCD
  help                    Show this help

Options:
  --debug,    -d      Debug mode (-O0 -g)
  --release,  -r      Release mode (-Os -DNDEBUG) [Default]
  --verbose,  -v      Show full SCons build output (flash output is always shown)

Environment:
  RTT_ROOT            RT-Thread source root      [$RTT_ROOT]
  RTT_EXEC_PATH       GNU Arm toolchain bin dir  [$RTT_EXEC_PATH]
  OPENOCD_INTERFACE   OpenOCD interface config   [$OPENOCD_INTERFACE]
  OPENOCD_TARGET      OpenOCD target config      [$OPENOCD_TARGET]

Examples:
  $0 build debug
  $0 rebuild -r
  $0 rebuild-flash -r
  OPENOCD_INTERFACE=cmsis-dap $0 flash
EOF
}

# ==============================================================================
# Main
# ==============================================================================

main() {
    [[ $# -eq 0 ]] && { show_help; exit 1; }

    local -a args=()
    VERBOSE=0
    for arg in "$@"; do
        case "$arg" in
            --verbose|-v)
                VERBOSE=1
                ;;
            --debug|-d|--release|-r|debug|release|help|--help|-h)
                args+=("$arg")
                ;;
            -*)
                die "Unsupported option: $arg"
                ;;
            *)
                args+=("$arg")
                ;;
        esac
    done

    if ((${#args[@]})); then
        set -- "${args[@]}"
    else
        set --
    fi

    [[ $# -eq 0 ]] && { show_help; exit 1; }

    local cmd="$1"
    shift

    case "$cmd" in
        build|rebuild|rebuild-flash)
            validate_mode_args "$cmd" "$@"
            ;;
        clean|flash|help|--help|-h)
            validate_no_args "$cmd" "$@"
            ;;
        *)
            die "Unknown command: $cmd"
            ;;
    esac

    case "$cmd" in
        build)              cmd_build "$@" ;;
        rebuild)            cmd_clean; cmd_build "$@" ;;
        rebuild-flash)      cmd_clean; cmd_build "$@"; cmd_flash ;;
        clean)              cmd_clean ;;
        flash)              cmd_flash ;;
        help|--help|-h)     show_help ;;
        *)                  die "Internal dispatch error: $cmd" ;;
    esac
}

main "$@"
