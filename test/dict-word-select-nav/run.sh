#!/usr/bin/env bash
set -euo pipefail

# Host-side build+run for the DictWordSelectNavigator litmus.
# Pure logic test: stubs replace GfxRenderer and MappedInputManager.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/dict-word-select-nav"
BINARY="$BUILD_DIR/DictWordSelectNavigatorTest"

mkdir -p "$BUILD_DIR"

CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR/test/dict-word-select-nav/stubs"  # host stubs first
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/EpdFont"
)

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/dict-word-select-nav/DictWordSelectNavigatorTest.cpp" \
  "$ROOT_DIR/src/util/WordSelectNavigator.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
