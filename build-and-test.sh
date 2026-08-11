#!/bin/bash
#
# CrossPoint Reader - the checks PlatformIO does not run.
#
#   ./build-and-test.sh                rustfmt, clippy, cargo test, C++ format  (seconds)
#   ./build-and-test.sh all            the same, then build everything CI builds
#   ./build-and-test.sh sim            build and run the simulator
#                                    (one-time setup in docs/simulator.md)
#   ./build-and-test.sh memory-report  flash/RAM vs budget + Rust's share
#                                    (builds only if there are no figures yet;
#                                     --rebuild forces fresh ones)
#   ./build-and-test.sh clean          wipe BOTH build trees - see below
#
# BUILDING AND FLASHING IS PLAIN PIO. Nothing here is required for it:
#
#   pio run                       build the default firmware
#   pio run -e sticky             build another environment
#   pio run -t upload             flash it
#
# There is no separate Rust step, and no order to remember. platformio.ini
# runs scripts/build_rust.py before every compile, so `pio run` compiles the
# Rust crates along with the C++. Same for the generated files: the i18n
# tables, HTML headers and Rust string constants are produced by the build.
# Add a key to lib/I18n/translations/english.yaml, use it, and build.
#
# cargo appears below only for the checks - never to produce firmware.
#
# `pio run` never deletes anything, and `pio run -t clean` only empties the
# PlatformIO tree for one environment - it knows nothing about cargo, so
# target/ survives. That is why `clean` exists here: two build systems, one
# command. Dropping target/ costs minutes on the next build (Xtensa has no
# prebuilt core/alloc), so reach for it only when a build makes no sense.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="/tmp/crosspoint-logs"
SIM_ENV="simulator_x3"
SIM_BINARY="$PROJECT_DIR/.pio/build/$SIM_ENV/program"

# The environments .github/workflows/ci.yml builds. Keep in step with it.
CI_ENVS=(default sticky)

# Homebrew's pio ships without the littlefs module the espressif32 builder
# needs, so prefer PlatformIO's own virtualenv when it is present.
if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  PIO="pio"
fi

# --- the checks --------------------------------------------------------------

rust_gates() {
  echo "==> Rust: format, lint, test"
  cd "$PROJECT_DIR"
  cargo fmt --check
  cargo clippy --workspace --all-targets --features xpui-rs/testing -- -D warnings
  cargo test --workspace --features xpui-rs/testing
}

# Everything above compiles for the HOST. Code behind `cfg(target_os = "none")`
# — the global allocator and the panic handler among it — is invisible there, so
# a lint or warning in it survives every check above and first shows up in a
# firmware build. That is exactly how an edition-2024 migration bug reached both
# device targets once already.
#
# ESP32-C3 stands in for both devices: the S3 compiles the same
# `target_os = "none"` code, and linting it too would need the Xtensa fork and a
# build-std of core and alloc for very little extra coverage.
device_lint() {
  echo "==> Rust: lint the device target (the host build cannot see this code)"
  cd "$PROJECT_DIR"
  cargo clippy --release --package crosspoint_rs \
    --target riscv32imc-unknown-none-elf -- -D warnings
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

checks() {
  rust_gates
  device_lint
  framework_is_generic
  cpp_format_check
}

usage() { sed -n '3,32p' "$0"; }

case "${1:-}" in
  "" | check)
    checks
    echo
    echo "Checks passed. './build-and-test.sh all' also builds every target."
    ;;

  all)
    checks
    # One pio invocation for every environment: a second `pio run` can wipe
    # .pio/build on a checksum mismatch and undo the first. See ci.yml.
    echo "==> Building the simulator and every environment CI builds"
    "$PIO" run -e "$SIM_ENV" $(printf -- '-e %s ' "${CI_ENVS[@]}")
    echo "==> Static analysis (cppcheck - the slow one)"
    "$PIO" check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
    echo
    echo "Everything CI checks has passed locally."
    ;;

  memory-report)
    # Figures come from a link. Build only when there are none to read - a
    # report should not cost a build every time you want to look at it. Pass
    # --rebuild to force fresh figures after changing code.
    shift || true
    REPORT_ENV="${MEMORY_REPORT_ENV:-default}"
    FORCE=""
    for arg in "$@"; do [ "$arg" = "--rebuild" ] && FORCE=1; done
    set -- "${@/--rebuild/}"
    if [ -n "$FORCE" ] || [ ! -f "$PROJECT_DIR/.pio/build/$REPORT_ENV/memory.json" ]; then
      echo "==> Building $REPORT_ENV for its figures"
      "$PIO" run -e "$REPORT_ENV"
    fi
    python3 "$PROJECT_DIR/scripts/report_memory.py" "$@"
    ;;

  clean)
    echo "==> Removing .pio/build and target/"
    rm -rf "$PROJECT_DIR/.pio/build" "$PROJECT_DIR/target"
    echo "Both build trees are gone. The next 'pio run' rebuilds from scratch."
    ;;

  sim)
    # The simulator is a second checkout, so its environments live in the
    # gitignored platformio.local.ini rather than in the shared config.
    if ! grep -qs "\[env:$SIM_ENV\]" "$PROJECT_DIR"/platformio*.ini; then
      echo "No '$SIM_ENV' environment is defined." >&2
      echo "The simulator needs a one-time platformio.local.ini - see docs/simulator.md." >&2
      exit 1
    fi
    "$PIO" run -e "$SIM_ENV"
    mkdir -p "$LOG_DIR"
    local_log="$LOG_DIR/sim_$(date +%Y%m%d_%H%M%S).log"
    echo
    echo "Simulator starting. Settings > System > Developers exercises the Rust screen."
    echo "Log: $local_log"
    echo "Ctrl+C to stop."
    echo
    "$SIM_BINARY" 2>&1 | tee "$local_log"
    ;;

  help | --help | -h)
    usage
    ;;

  *)
    echo "Unknown mode: $1" >&2
    usage
    exit 1
    ;;
esac
