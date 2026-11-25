#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/crosspoint-clipping-sync-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
json_dir="${ARDUINOJSON_DIR:-.pio/libdeps/default/ArduinoJson/src}"
for chip in ESP32C3 ESP32S3; do
  c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions \
    "-DCONFIG_IDF_TARGET_$chip=1" -I"$json_dir" \
    -Itest/clipping_sync/stubs -Itest/clipping_store/stubs -Itest/library_builder/stubs \
    -Isrc -Ilib/Memory -Ilib/Serialization -Ilib/Utf8 \
    test/clipping_sync/ClippingSyncTest.cpp src/clippings/ClippingSync.cpp src/sync/SyncRequest.cpp \
    src/ClippingStore.cpp lib/Utf8/Utf8.cpp lib/Memory/BuildScratch.cpp \
    -o "$build_dir/$chip"
  "$build_dir/$chip"
done
