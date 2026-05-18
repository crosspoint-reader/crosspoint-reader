#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

forbidden_changed_paths='^(lib/WorkInterrupt/|lib/GfxRenderer/|src/LoopPowerSavingPolicy\.h|src/MappedInputManager\.h|src/main\.cpp|src/activities/reader/TxtReaderActivity\.|lib/hal/HalGPIO\.)'
allowed_sdk_path='libs/display/EInkDisplay/src/EInkDisplay.cpp'

resolve_base_ref() {
  if [[ -n "${INCREMENTAL_SECTION_BASE_REF:-}" ]]; then
    printf '%s\n' "$INCREMENTAL_SECTION_BASE_REF"
    return
  fi
  if git -C "$ROOT_DIR" rev-parse --verify --quiet origin/master >/dev/null; then
    printf '%s\n' "origin/master"
    return
  fi
  if git -C "$ROOT_DIR" rev-parse --verify --quiet master >/dev/null; then
    printf '%s\n' "master"
    return
  fi
  echo "Could not resolve base ref; set INCREMENTAL_SECTION_BASE_REF" >&2
  exit 1
}

BASE_REF="$(resolve_base_ref)"
BASE_COMMIT="$(git -C "$ROOT_DIR" merge-base "$BASE_REF" HEAD)"

committed_changed_paths="$(git -C "$ROOT_DIR" diff --name-only "$BASE_COMMIT"..HEAD -- . \
  ':!test/incremental_section/CleanScopeContractTest.sh' \
  ':!open-x4-sdk')"
working_changed_paths="$(git -C "$ROOT_DIR" diff --name-only -- . \
  ':!test/incremental_section/CleanScopeContractTest.sh' \
  ':!open-x4-sdk')"

changed_paths="$(printf '%s\n%s\n' "$committed_changed_paths" "$working_changed_paths" | sed '/^$/d' | sort -u)"

if printf '%s\n' "$changed_paths" | rg -q "$forbidden_changed_paths"; then
  echo "Incremental EPUB reimplementation touched forbidden debugging, power, input, TXT, graphics, HAL GPIO, or SDK scope" >&2
  printf '%s\n' "$changed_paths" | rg "$forbidden_changed_paths" >&2
  exit 1
fi

submodule_commit_at() {
  git -C "$ROOT_DIR" ls-tree "$1" open-x4-sdk | awk '{print $3}'
}

ensure_sdk_commit_available() {
  local commit="$1"
  if [[ -z "$commit" ]] || ! git -C "$ROOT_DIR/open-x4-sdk" cat-file -e "$commit^{commit}" 2>/dev/null; then
    echo "Cannot inspect open-x4-sdk diff for commit: ${commit:-missing}" >&2
    exit 1
  fi
}

inspect_sdk_range() {
  local old_commit="$1"
  local new_commit="$2"
  if [[ "$old_commit" == "$new_commit" ]]; then
    return
  fi
  ensure_sdk_commit_available "$old_commit"
  ensure_sdk_commit_available "$new_commit"

  local sdk_changed_paths
  sdk_changed_paths="$(git -C "$ROOT_DIR/open-x4-sdk" diff --name-only "$old_commit" "$new_commit")"
  if [[ -n "$sdk_changed_paths" ]] && printf '%s\n' "$sdk_changed_paths" | rg -vq "^$allowed_sdk_path$"; then
    echo "Only the EInkDisplay AA cleanup is allowed under open-x4-sdk for this feature" >&2
    printf '%s\n' "$sdk_changed_paths" >&2
    exit 1
  fi
}

base_sdk_commit="$(submodule_commit_at "$BASE_COMMIT")"
head_sdk_commit="$(submodule_commit_at HEAD)"
inspect_sdk_range "$base_sdk_commit" "$head_sdk_commit"

if git -C "$ROOT_DIR" diff --name-only -- open-x4-sdk | rg -q '^open-x4-sdk$'; then
  worktree_sdk_commit="$(git -C "$ROOT_DIR/open-x4-sdk" rev-parse HEAD)"
  inspect_sdk_range "$head_sdk_commit" "$worktree_sdk_commit"
fi

sdk_dirty_paths="$(git -C "$ROOT_DIR/open-x4-sdk" diff --name-only)"
if [[ -n "$sdk_dirty_paths" ]] && printf '%s\n' "$sdk_dirty_paths" | rg -vq "^$allowed_sdk_path$"; then
  echo "Only the EInkDisplay AA cleanup is allowed under open-x4-sdk for this feature" >&2
  printf '%s\n' "$sdk_dirty_paths" >&2
  exit 1
fi

combined_diff="$(git -C "$ROOT_DIR" diff "$BASE_COMMIT"..HEAD -- . ':!test/incremental_section/CleanScopeContractTest.sh' ':!open-x4-sdk'
git -C "$ROOT_DIR" diff -- . ':!test/incremental_section/CleanScopeContractTest.sh' ':!open-x4-sdk')"
if printf '%s\n' "$combined_diff" | rg -q 'WorkInterrupt|pollAndQueueEvents|LoopPowerSavingPolicy|ReaderWorkActivity|PageTurnDeferral|InputManagerQueuedEvents'; then
  echo "Incremental EPUB reimplementation reintroduced previous input/power/debug scaffolding" >&2
  exit 1
fi

gh_release_block="$(awk '
  /^\[env:gh_release\]$/ { in_block = 1; next }
  /^\[env:/ { if (in_block) exit }
  in_block { print }
' "$ROOT_DIR/platformio.ini")"
if ! printf '%s\n' "$gh_release_block" | rg -q 'LOG_LEVEL=0'; then
  echo "gh_release must build with LOG_LEVEL=0 for performance verification" >&2
  exit 1
fi
