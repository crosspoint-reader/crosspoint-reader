#!/usr/bin/env bash
# Build and run host-side tests against lib/Pdf (real parser sources).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="test/pdf/build"
mkdir -p "$OUT_DIR"

CXX="${CXX:-g++}"
STD="${CXXSTD:--std=c++20}"

# Stub HalStorage must appear before lib/hal on the include path.
INCLUDES=(
  -I "test/pdf/stubs"
  -I "lib"
  -I "lib/Serialization"
  -I "lib/Pdf"
  -I "lib/Pdf/Pdf"
  -I "src/activities/reader"
  -I "lib/InflateReader"
  -I "lib/uzlib/src"
)

CC="${CC:-cc}"
$CC -c -O2 -I "lib/uzlib/src" "lib/uzlib/src/tinflate.c" -o "$OUT_DIR/tinflate.o"
$CC -c -O2 "test/pdf/stubs/uzlib_checksum_stubs.c" -o "$OUT_DIR/uzlib_checksum_stubs.o"

SOURCES=(
  "lib/Pdf/Pdf/PdfCachedPageReader.cpp"
  "lib/Pdf/Pdf/XrefTable.cpp"
  "lib/Pdf/Pdf/PdfObject.cpp"
  "lib/Pdf/Pdf/PageTree.cpp"
  "lib/Pdf/Pdf/StreamDecoder.cpp"
  "lib/Pdf/Pdf/ContentStream.cpp"
  "lib/Pdf/Pdf/PdfOutline.cpp"
  "lib/InflateReader/InflateReader.cpp"
  "test/pdf/pdf_parser_host_test.cpp"
)

$CXX $STD -Wall -Wextra -O2 "${INCLUDES[@]}" "${SOURCES[@]}" "$OUT_DIR/tinflate.o" "$OUT_DIR/uzlib_checksum_stubs.o" \
  -o "$OUT_DIR/pdf_parser_host_test"

if [[ $# -eq 0 ]]; then
  set -- "$ROOT/test/pdf/sample.pdf" "$ROOT/test/pdf/EE-366.pdf" "$ROOT/test/pdf/esp32-c6_datasheet_en.pdf" \
    "$ROOT/test/pdf/Problem-Solving Treatment_ Learning and Pl - IHS.pdf"
fi

echo "Running PDF parser host tests..."
"$OUT_DIR/pdf_parser_host_test" "$@"
