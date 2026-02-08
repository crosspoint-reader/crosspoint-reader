#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/unit_tests"
STUB_DIR="$ROOT_DIR/test/stubs"

mkdir -p "$BUILD_DIR"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$STUB_DIR"
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/Utf8"
  -I"$ROOT_DIR/lib/Epub"
  -I"$ROOT_DIR/src"
)

CFLAGS=(
  -std=c11
  -O2
  -w
  -I"$ROOT_DIR/lib"
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
)

PASSED=0
FAILED=0
ERRORS=""

compile_and_run() {
  local name="$1"
  shift
  local sources=("$@")

  echo "--- $name ---"
  if c++ "${CXXFLAGS[@]}" "${sources[@]}" -o "$BUILD_DIR/$name" 2>&1; then
    if "$BUILD_DIR/$name"; then
      PASSED=$((PASSED + 1))
    else
      FAILED=$((FAILED + 1))
      ERRORS="$ERRORS  $name\n"
    fi
  else
    echo "  COMPILE ERROR"
    FAILED=$((FAILED + 1))
    ERRORS="$ERRORS  $name (compile error)\n"
  fi
  echo ""
}

# --- Utf8Test ---
compile_and_run Utf8Test \
  "$ROOT_DIR/test/unit/Utf8Test.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp"

# --- UrlUtilsTest ---
compile_and_run UrlUtilsTest \
  "$ROOT_DIR/test/unit/UrlUtilsTest.cpp" \
  "$ROOT_DIR/src/util/UrlUtils.cpp"

# --- StringUtilsTest ---
compile_and_run StringUtilsTest \
  "$ROOT_DIR/test/unit/StringUtilsTest.cpp" \
  "$ROOT_DIR/src/util/StringUtils.cpp"

# --- SerializationTest ---
compile_and_run SerializationTest \
  "$ROOT_DIR/test/unit/SerializationTest.cpp"

# --- BlockStyleTest ---
compile_and_run BlockStyleTest \
  "$ROOT_DIR/test/unit/BlockStyleTest.cpp"

# --- CssParserTest ---
compile_and_run CssParserTest \
  "$ROOT_DIR/test/unit/CssParserTest.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/css/CssParser.cpp"

# --- CssParserRegressionTest ---
compile_and_run CssParserRegressionTest \
  "$ROOT_DIR/test/unit/CssParserRegressionTest.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/css/CssParser.cpp"

# --- HyphenationRegressionTest ---
compile_and_run HyphenationRegressionTest \
  "$ROOT_DIR/test/unit/HyphenationRegressionTest.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/Hyphenator.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp" \
  "$ROOT_DIR/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp"

# --- OpdsParserTest ---
# Expat is C code — compile as object files first, then link with C++ test
echo "--- OpdsParserTest ---"
OPDS_OK=true
for csrc in xmlparse.c xmlrole.c xmltok.c; do
  if ! cc "${CFLAGS[@]}" -c "$ROOT_DIR/lib/expat/$csrc" -o "$BUILD_DIR/${csrc%.c}.o" 2>&1; then
    echo "  COMPILE ERROR ($csrc)"
    OPDS_OK=false
    break
  fi
done

if $OPDS_OK; then
  if c++ "${CXXFLAGS[@]}" \
    "$ROOT_DIR/test/unit/OpdsParserTest.cpp" \
    "$ROOT_DIR/lib/OpdsParser/OpdsParser.cpp" \
    "$BUILD_DIR/xmlparse.o" "$BUILD_DIR/xmlrole.o" "$BUILD_DIR/xmltok.o" \
    -o "$BUILD_DIR/OpdsParserTest" 2>&1; then
    if "$BUILD_DIR/OpdsParserTest"; then
      PASSED=$((PASSED + 1))
    else
      FAILED=$((FAILED + 1))
      ERRORS="$ERRORS  OpdsParserTest\n"
    fi
  else
    echo "  COMPILE ERROR (link)"
    FAILED=$((FAILED + 1))
    ERRORS="$ERRORS  OpdsParserTest (compile error)\n"
  fi
else
  FAILED=$((FAILED + 1))
  ERRORS="$ERRORS  OpdsParserTest (compile error)\n"
fi
echo ""

# --- Summary ---
echo "==============================="
echo "Unit tests: $PASSED passed, $FAILED failed"
if [ "$FAILED" -gt 0 ]; then
  echo -e "Failed:\n$ERRORS"
  exit 1
fi
echo "All tests passed."
