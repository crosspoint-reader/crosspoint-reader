#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT/test/ota_version/build"
mkdir -p "$OUT_DIR"

c++ -std=c++20 -Wall -Wextra -pedantic -I "$ROOT/src" "$ROOT/test/ota_version/ota_version_host_test.cpp" \
  -o "$OUT_DIR/ota_version_host_test"

"$OUT_DIR/ota_version_host_test"
