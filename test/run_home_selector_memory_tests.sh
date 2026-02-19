#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/home_selector_memory_tests"
BINARY="$BUILD_DIR/HomeSelectorMemoryTest"

mkdir -p "$BUILD_DIR"

if [ -n "${CXX:-}" ]; then
  CXX_BIN="$CXX"
elif command -v g++ >/dev/null 2>&1; then
  CXX_BIN="g++"
else
  CXX_BIN="c++"
fi

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

"$CXX_BIN" "${CXXFLAGS[@]}" "$ROOT_DIR/test/home_selector_memory/HomeSelectorMemoryTest.cpp" -o "$BINARY"

"$BINARY"
