#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/writer_text_slice"
BINARY="$BUILD_DIR/WriterTextSliceTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/writer_text_slice/WriterTextSliceTest.cpp"
  "$ROOT_DIR/src/activities/writer/WriterTextSlice.cpp"
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
