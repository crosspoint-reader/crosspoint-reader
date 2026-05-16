#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

forbidden_changed_paths='^(lib/WorkInterrupt/|lib/GfxRenderer/|src/LoopPowerSavingPolicy\.h|src/MappedInputManager\.h|src/main\.cpp|src/activities/reader/TxtReaderActivity\.|lib/hal/HalGPIO\.)'
allowed_sdk_path='libs/display/EInkDisplay/src/EInkDisplay.cpp'

changed_paths="$(git -C "$ROOT_DIR" diff --name-only -- . \
  ':!test/incremental_section/CleanScopeContractTest.sh' \
  ':!open-x4-sdk')"

if printf '%s\n' "$changed_paths" | rg -q "$forbidden_changed_paths"; then
  echo "Incremental EPUB reimplementation touched forbidden debugging, power, input, TXT, graphics, HAL GPIO, or SDK scope" >&2
  printf '%s\n' "$changed_paths" | rg "$forbidden_changed_paths" >&2
  exit 1
fi

if git -C "$ROOT_DIR" diff --name-only -- open-x4-sdk | rg -q '^open-x4-sdk$'; then
  sdk_changed_paths="$(git -c safe.directory="$ROOT_DIR/open-x4-sdk" -C "$ROOT_DIR/open-x4-sdk" diff --name-only)"
  if [[ -n "$sdk_changed_paths" ]] && printf '%s\n' "$sdk_changed_paths" | rg -vq "^$allowed_sdk_path$"; then
    echo "Only the EInkDisplay AA cleanup is allowed under open-x4-sdk for this feature" >&2
    printf '%s\n' "$sdk_changed_paths" >&2
    exit 1
  fi
fi

if git -C "$ROOT_DIR" diff -- . ':!test/incremental_section/CleanScopeContractTest.sh' | rg -q 'WorkInterrupt|pollAndQueueEvents|LoopPowerSavingPolicy|ReaderWorkActivity|PageTurnDeferral|InputManagerQueuedEvents'; then
  echo "Incremental EPUB reimplementation reintroduced previous input/power/debug scaffolding" >&2
  exit 1
fi

if ! rg -q 'LOG_LEVEL=0' "$ROOT_DIR/platformio.ini"; then
  echo "gh_release must build with LOG_LEVEL=0 for performance verification" >&2
  exit 1
fi
