#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/perf_tests"
STUB_DIR="$ROOT_DIR/test/stubs"

mkdir -p "$BUILD_DIR"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -Wno-unused-but-set-variable
  -I"$STUB_DIR"
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/Utf8"
  -I"$ROOT_DIR/lib/Epub"
  -I"$ROOT_DIR/src"
)

echo "=== Performance Benchmarks ==="
echo ""

# --- HyphenationBench ---
echo "--- HyphenationBench ---"
c++ "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/perf/HyphenationBench.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/Hyphenator.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp" \
  -o "$BUILD_DIR/HyphenationBench"
"$BUILD_DIR/HyphenationBench"
echo ""

# --- CssParserBench ---
echo "--- CssParserBench ---"
c++ "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/perf/CssParserBench.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/css/CssParser.cpp" \
  -o "$BUILD_DIR/CssParserBench"
"$BUILD_DIR/CssParserBench"
echo ""

echo "=== Benchmarks complete ==="
