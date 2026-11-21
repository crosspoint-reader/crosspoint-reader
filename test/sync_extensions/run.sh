#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir="${TMPDIR:-/tmp}/crosspoint-sync-extensions-tests"
json_dir="${ARDUINOJSON_DIR:-.pio/libdeps/default/ArduinoJson/src}"
mkdir -p "$build_dir"
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions -I"$json_dir" \
  -Itest/sync_extensions/stubs -Itest/clipping_sync/stubs -Itest/library_builder/stubs \
  -Itest/clipping_store/stubs -Isrc -Ilib/Memory -Ilib/KOReaderSync -Ilib/Serialization -Ilib/Utf8 \
  test/sync_extensions/SyncExtensionsTest.cpp src/sync/SyncEngine.cpp src/sync/SyncRequest.cpp src/sync/BookmarkSync.cpp \
  src/clippings/ClippingSync.cpp src/ClippingStore.cpp lib/Utf8/Utf8.cpp \
  src/sync/ReadingSessionLog.cpp src/util/BookmarkFile.cpp src/util/BookmarkUtil.cpp lib/Memory/BuildScratch.cpp \
  -o "$build_dir/test"
"$build_dir/test"
