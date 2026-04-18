#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/river_remainder"
BINARY="$BUILD_DIR/JustifyRemainderAllocatorTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/river_remainder/JustifyRemainderAllocatorTest.cpp"
  "$ROOT_DIR/test/river_remainder/JustifyRemainderAllocator.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
