#!/usr/bin/env bash
set -euo pipefail

# Host-side build+run for the DictLayout (wrap/pagination) litmus.
# Pure logic test: no expat execution, no fonts, no SD — DictLayout uses
# StyledSpan only as a struct and EpdFontFamily::Style only as an enum.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/dict-layout"
BINARY="$BUILD_DIR/DictLayoutTest"

mkdir -p "$BUILD_DIR"

CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/expat"
  -I"$ROOT_DIR/lib/EpdFont"
  -I"$ROOT_DIR/lib/Utf8"
  -I"$ROOT_DIR/lib/DictHtmlRenderer"
  -I"$ROOT_DIR/src/util"
)

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/dict-layout/DictLayoutTest.cpp" \
  "$ROOT_DIR/src/util/DictLayout.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
