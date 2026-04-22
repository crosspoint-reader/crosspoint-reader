#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/writer_visible_lines"
BINARY="$BUILD_DIR/WriterVisibleLinesTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/writer_visible_lines/WriterVisibleLinesTest.cpp"
  "$ROOT_DIR/src/activities/writer/WriterVisibleLines.cpp"
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
