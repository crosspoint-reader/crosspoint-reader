#!/usr/bin/env bash
set -euo pipefail

# Host-side build+run for the LookupChain litmus. Pure logic — header-only class,
# no SD/settings/expat.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/lookup-chain"
BINARY="$BUILD_DIR/LookupChainTest"

mkdir -p "$BUILD_DIR"

CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
)

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/lookup-chain/LookupChainTest.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
