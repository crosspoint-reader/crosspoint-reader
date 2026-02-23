#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST_TEST_DIR="$ROOT_DIR/test/host"
BUILD_DIR="$ROOT_DIR/build/host_tests"

mkdir -p "$BUILD_DIR"

if [ -n "${CXX:-}" ]; then
  CXX_BIN="$CXX"
elif command -v g++ >/dev/null 2>&1; then
  CXX_BIN="g++"
else
  CXX_BIN="c++"
fi

COMMON_CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
)

trim_line() {
  local line="$1"
  line="${line%%#*}"                    # Strip comments.
  line="${line#"${line%%[![:space:]]*}"}"  # Trim leading spaces.
  line="${line%"${line##*[![:space:]]}"}"  # Trim trailing spaces.
  printf '%s' "$line"
}

load_manifest_lines() {
  local manifest_path="$1"
  local output_var_name="$2"
  local -n output_ref="$output_var_name"

  if [ ! -f "$manifest_path" ]; then
    return
  fi

  local raw_line
  local cleaned
  while IFS= read -r raw_line; do
    cleaned="$(trim_line "$raw_line")"
    if [ -z "$cleaned" ]; then
      continue
    fi
    output_ref+=("$cleaned")
  done < "$manifest_path"
}

shopt -s nullglob globstar
TEST_FILES=("$HOST_TEST_DIR"/**/*Test.cpp)
shopt -u nullglob globstar

if [ "${#TEST_FILES[@]}" -eq 0 ]; then
  echo "No host tests found in $HOST_TEST_DIR"
  exit 1
fi

FAILED=0
PASSED=0

for test_file in "${TEST_FILES[@]}"; do
  rel_test_path="${test_file#"$ROOT_DIR"/}"
  test_base="${test_file%.cpp}"
  binary_name="${rel_test_path%.cpp}"
  binary_name="${binary_name//\//_}"
  binary_path="$BUILD_DIR/$binary_name"

  sources=("$test_file")
  extra_sources=()
  extra_flags=()

  load_manifest_lines "${test_base}.sources" extra_sources
  load_manifest_lines "${test_base}.cxxflags" extra_flags

  for source in "${extra_sources[@]}"; do
    sources+=("$ROOT_DIR/$source")
  done

  echo "-------------------------------------------------------------------------------"
  echo "Building host test: $rel_test_path"
  if ! "$CXX_BIN" "${COMMON_CXXFLAGS[@]}" "${extra_flags[@]}" "${sources[@]}" -o "$binary_path"; then
    FAILED=$((FAILED + 1))
    continue
  fi

  echo "Running host test:  $rel_test_path"
  if "$binary_path"; then
    PASSED=$((PASSED + 1))
  else
    FAILED=$((FAILED + 1))
  fi
done

echo "-------------------------------------------------------------------------------"
echo "Host test summary: $PASSED passed, $FAILED failed"

if [ "$FAILED" -gt 0 ]; then
  exit 1
fi
