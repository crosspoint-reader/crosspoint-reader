#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/html_to_text"
BINARY="$BUILD_DIR/HtmlToTextTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/html_to_text/HtmlToTextTest.cpp"
  "$ROOT_DIR/src/network/HtmlToText.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
