#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/streaming_json_parser"
BINARY="$BUILD_DIR/StreamingJsonParserTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/streaming_json_parser/StreamingJsonParserTest.cpp"
  "$ROOT_DIR/lib/JsonParser/StreamingJsonParser.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/JsonParser"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
