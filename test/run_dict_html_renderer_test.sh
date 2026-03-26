#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/dict_html_renderer"
BINARY="$BUILD_DIR/DictHtmlRendererTest"

mkdir -p "$BUILD_DIR"

COMMON_DEFINES=(
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
)

CFLAGS=(
  -std=c11
  -O2
  -Wall
  "${COMMON_DEFINES[@]}"
  -I"$ROOT_DIR/lib/expat"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -DDICT_HTML_RENDERER_TRACK_UNKNOWN
  "${COMMON_DEFINES[@]}"
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/expat"
  -I"$ROOT_DIR/lib/Utf8"
)

# Compile expat C sources with gcc
gcc "${CFLAGS[@]}" -c "$ROOT_DIR/lib/expat/xmlparse.c" -o "$BUILD_DIR/xmlparse.o"
gcc "${CFLAGS[@]}" -c "$ROOT_DIR/lib/expat/xmlrole.c"  -o "$BUILD_DIR/xmlrole.o"
gcc "${CFLAGS[@]}" -c "$ROOT_DIR/lib/expat/xmltok.c"   -o "$BUILD_DIR/xmltok.o"

# Compile and link C++ sources + expat objects
g++ "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/dict_html_renderer/DictHtmlRendererTest.cpp" \
  "$ROOT_DIR/lib/DictHtmlRenderer/DictHtmlRenderer.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp" \
  "$BUILD_DIR/xmlparse.o" \
  "$BUILD_DIR/xmlrole.o" \
  "$BUILD_DIR/xmltok.o" \
  -o "$BINARY"

"$BINARY" "$ROOT_DIR/test/dictionaries/expat-full" "$@"
