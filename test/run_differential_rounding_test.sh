#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/differential_rounding"
BINARY="$BUILD_DIR/DifferentialRoundingTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/differential_rounding/DifferentialRoundingTest.cpp"
  "$ROOT_DIR/lib/EpdFont/EpdFont.cpp"
  "$ROOT_DIR/lib/Utf8/Utf8.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/EpdFont"
  -I"$ROOT_DIR/lib/Utf8"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
