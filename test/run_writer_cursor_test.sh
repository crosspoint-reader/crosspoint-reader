#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/writer_cursor"
BINARY="$BUILD_DIR/WriterCursorTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/writer_cursor/WriterCursorTest.cpp"
  "$ROOT_DIR/src/activities/writer/WriterCursor.cpp"
  "$ROOT_DIR/lib/Utf8/Utf8.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib/Utf8"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
