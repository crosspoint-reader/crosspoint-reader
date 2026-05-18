#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build/incremental_section"

cd "$ROOT_DIR"
mkdir -p "$BUILD_DIR"

bash "$ROOT_DIR/test/incremental_section/CleanScopeContractTest.sh"
if command -v python >/dev/null 2>&1; then
  PYTHON=python
elif command -v python3 >/dev/null 2>&1; then
  PYTHON=python3
else
  echo "No Python interpreter found; cannot run static incremental contracts."
  exit 1
fi
"$PYTHON" "$ROOT_DIR/test/incremental_section/StaticIncrementalContracts.py"

if command -v c++ >/dev/null 2>&1; then
  CXX=c++
elif command -v g++ >/dev/null 2>&1; then
  CXX=g++
elif command -v clang++ >/dev/null 2>&1; then
  CXX=clang++
elif [ -x /c/msys64/ucrt64/bin/c++.exe ]; then
  CXX=/c/msys64/ucrt64/bin/c++.exe
  PATH="/c/msys64/ucrt64/bin:$PATH"
else
  echo "No host C++ compiler found; static incremental contracts passed, skipping compile-only host tests."
  exit 0
fi

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/src"
  -I"$ROOT_DIR/test/incremental_section"
)

build_and_run() {
  local source_file="$1"
  local binary_name="$2"
  local binary="$BUILD_DIR/$binary_name"

  "$CXX" "${CXXFLAGS[@]}" "$ROOT_DIR/test/incremental_section/$source_file" -o "$binary"
  "$binary"
}

build_and_run IncrementalSectionTypesTest.cpp IncrementalSectionTypesTest
build_and_run EpubIndexingPolicyTest.cpp EpubIndexingPolicyTest
build_and_run ReaderProgressPolicyTest.cpp ReaderProgressPolicyTest
build_and_run StatusPageInfoTest.cpp StatusPageInfoTest
