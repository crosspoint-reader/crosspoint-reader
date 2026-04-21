#!/usr/bin/env bash

# Check if clang-format is available and pick the preferred binary.
if command -v clang-format-21 >/dev/null 2>&1; then
  CLANG_FORMAT_BIN="clang-format-21"
elif command -v clang-format >/dev/null 2>&1; then
  CLANG_FORMAT_BIN="clang-format"
else
  printf "'clang-format' not found in current environment\n"
  printf "Install clang-format-21 (recommended), clang, clang-tools, or clang-format depending on your distro/os and tooling requirements\n"
  exit 1
fi

set -euo pipefail

GIT_LS_FILES_FLAGS=""
# -g scopes formatting to tracked files currently modified in git status.
if [[ "${1:-}" == "-g" ]]; then
  GIT_LS_FILES_FLAGS="--modified"
fi

CLANG_FORMAT_VERSION_RAW="$(${CLANG_FORMAT_BIN} --version)"
CLANG_FORMAT_MAJOR="$(printf '%s\n' "${CLANG_FORMAT_VERSION_RAW}" | grep -oE '[0-9]+' | head -n1)"

# Guard against local binaries older than the repo formatting config.
if [[ -z "${CLANG_FORMAT_MAJOR}" || "${CLANG_FORMAT_MAJOR}" -lt 21 ]]; then
  echo "Error: ${CLANG_FORMAT_BIN} is too old: ${CLANG_FORMAT_VERSION_RAW}"
  echo "This repository's .clang-format requires clang-format 21 or newer."
  echo "Install clang-format-21 and rerun ./bin/clang-format-fix"
  exit 1
fi

# --- Main Logic ---

# Format all files (or only modified files if -g is passed)

# Use 'git ls-files' to get a list of all files tracked by git:
# --modified: files tracked by git that have been modified (staged or unstaged)
# --exclude-standard: ignores files in .gitignore
# Additionally exclude files in 'lib/EpdFont/builtinFonts/' as they are script-generated.
# Also exclude files in 'lib/Epub/Epub/hyphenation/generated/' as they are script-generated.
# Keep the no-match case non-fatal: grep returns 1 when no files match,
# which is expected when there are no modified C/C++ files.
set +o pipefail
git ls-files  --exclude-standard ${GIT_LS_FILES_FLAGS} \
    | grep -E '\.(c|cpp|h|hpp)$' \
    | grep -v -E '^lib/EpdFont/builtinFonts/' \
    | grep -v -E '^lib/Epub/Epub/hyphenation/generated/' \
    | grep -v -E '^lib/uzlib/' \
    | xargs -r "${CLANG_FORMAT_BIN}" -style=file -i
# Restore strict pipeline failure handling for the rest of the script.
set -o pipefail
