#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/hyphenation_eval"
BINARY="$BUILD_DIR/HyphenationEvaluationTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/hyphenation_eval/HyphenationEvaluationTest.cpp"
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/Hyphenator.cpp"
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp"
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp"
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp"
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
  -I"$ROOT_DIR/lib/Utf8"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
