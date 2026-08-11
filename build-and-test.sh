#!/bin/bash
#
# CrossPoint Reader - build and test helper.
#
#   ./build-and-test.sh            gates, build the simulator, run it   (inner loop)
#   ./build-and-test.sh check      the fast gates, no build             (seconds)
#   ./build-and-test.sh all        everything CI does                   (before you push)
#   ./build-and-test.sh run        run the existing simulator binary
#   ./build-and-test.sh device     gates, then build the x4pro firmware
#   ./build-and-test.sh clean      wipe every build artefact, then the inner loop
#
# Nothing here needs generating by hand. The i18n tables, HTML headers and Rust
# string constants are produced by the build itself: cargo through
# lib/backend_rs/build.rs, PlatformIO through scripts/. Add a key to
# lib/I18n/translations/english.yaml, use it, and build.
#
# Rust is compiled by PlatformIO through scripts/build_rust.py, so cargo is
# never invoked here for the firmware itself - only for the quality gates.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="/tmp/crosspoint-logs"
SIM_ENV="simulator_x4_pro"
DEVICE_ENV="x4pro"
SIM_BINARY="$PROJECT_DIR/.pio/build/$SIM_ENV/program"

# The environments .github/workflows/ci.yml builds. Keep in step with it.
CI_ENVS=(default sticky x4pro)

# Homebrew's pio ships without the littlefs module the espressif32 builder
# needs, so prefer PlatformIO's own virtualenv when it is present.
if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  PIO="pio"
fi

mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/test_$(date +%Y%m%d_%H%M%S).log"

# --- gates -------------------------------------------------------------------

rust_gates() {
  echo "==> Rust: format, lint, test"
  cd "$PROJECT_DIR"
  cargo fmt --check
  cargo clippy --workspace --all-targets --features xpui-rs/testing -- -D warnings
  cargo test --workspace --features xpui-rs/testing
}

# xpui_rs is the generic framework: it must not name the product. CI fails on
# this, so failing here too keeps local and CI in agreement.
framework_is_generic() {
  echo "==> xpui_rs carries no product vocabulary"
  cd "$PROJECT_DIR"
  if grep -rniE "crosspoint|xteink" lib/xpui_rs/src/; then
    echo "ERROR: xpui_rs must stay generic - move the above into crosspoint_rs." >&2
    return 1
  fi
}

# Reports without rewriting anything; ./bin/clang-format-fix is the fix.
cpp_format_check() {
  echo "==> C++ formatting"
  cd "$PROJECT_DIR"
  local bin
  if command -v clang-format-21 >/dev/null 2>&1; then
    bin=clang-format-21
  elif command -v clang-format >/dev/null 2>&1; then
    bin=clang-format
  else
    echo "  skipped: clang-format not installed (CI will still check this)"
    return 0
  fi

  local major
  major="$("$bin" --version | grep -oE '[0-9]+' | head -n1)"
  if [ -z "$major" ] || [ "$major" -lt 21 ]; then
    echo "  skipped: $bin is older than the .clang-format requires (need 21+)"
    return 0
  fi

  # Same selection as bin/clang-format-fix, but --dry-run instead of -i.
  set +o pipefail
  local files
  files="$(git ls-files --exclude-standard \
    | grep -E '\.(c|cpp|h|hpp)$' \
    | grep -v -E '^lib/EpdFont/builtinFonts/' \
    | grep -v -E '^lib/Epub/Epub/hyphenation/generated/' \
    | grep -v -E '^lib/uzlib/' \
    | grep -v -E '^lib/miniz/third_party/')"
  set -o pipefail

  if ! printf '%s\n' "$files" | xargs -r "$bin" -style=file --dry-run -Werror 2>&1; then
    echo "ERROR: formatting differs. Fix with: ./bin/clang-format-fix" >&2
    return 1
  fi
}

fast_gates() {
  rust_gates
  framework_is_generic
  cpp_format_check
}

# --- actions -----------------------------------------------------------------

run_simulator() {
  echo
  echo "Simulator starting. Navigate to Settings > About to exercise the Rust screen."
  echo "Log: $LOG_FILE"
  echo "Ctrl+C to stop."
  echo
  "$SIM_BINARY" 2>&1 | tee "$LOG_FILE"
}

case "${1:-default}" in
  default)
    fast_gates
    echo "==> Building $SIM_ENV"
    "$PIO" run -e "$SIM_ENV"
    run_simulator
    ;;

  check)
    fast_gates
    echo
    echo "All gates passed. './build-and-test.sh all' also builds every target."
    ;;

  all)
    fast_gates
    # One pio invocation for every environment: a second `pio run` can wipe
    # .pio/build on a checksum mismatch and undo the first. See ci.yml.
    echo "==> Building the simulator and every environment CI builds"
    "$PIO" run -e "$SIM_ENV" $(printf -- '-e %s ' "${CI_ENVS[@]}")
    echo "==> Static analysis (cppcheck - the slow one)"
    "$PIO" check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
    echo
    echo "Everything CI checks has passed locally."
    ;;

  run)
    if [ ! -x "$SIM_BINARY" ]; then
      echo "ERROR: no simulator binary. Run './build-and-test.sh' first." >&2
      exit 1
    fi
    run_simulator
    ;;

  device)
    fast_gates
    echo "==> Building $DEVICE_ENV firmware"
    "$PIO" run -e "$DEVICE_ENV"
    echo "Firmware built. Flash with: $PIO run -e $DEVICE_ENV -t upload"
    ;;

  clean)
    fast_gates
    # target/ holds the Rust build cache, including build-std for Xtensa;
    # dropping it costs minutes and is only needed when cargo itself misbehaves.
    echo "==> Cleaning build tree"
    rm -rf "$PROJECT_DIR/.pio/build" "$PROJECT_DIR/target"
    echo "==> Building $SIM_ENV"
    "$PIO" run -e "$SIM_ENV"
    run_simulator
    ;;

  help | --help | -h)
    sed -n '3,10p' "$0"
    ;;

  *)
    echo "Unknown mode: $1" >&2
    sed -n '3,10p' "$0"
    exit 1
    ;;
esac
