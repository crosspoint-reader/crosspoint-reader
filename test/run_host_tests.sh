#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/host_tests"
ARDUINOJSON_DIR="$ROOT_DIR/.pio/libdeps/default/ArduinoJson/src"

mkdir -p "$BUILD_DIR"

if [ ! -d "$ARDUINOJSON_DIR" ] && command -v pio >/dev/null 2>&1; then
  echo "Bootstrapping ArduinoJson for host tests..."
  (
    cd "$ROOT_DIR"
    pio pkg install -e default --library "bblanchon/ArduinoJson@7.4.2"
  )
fi

if [ ! -d "$ARDUINOJSON_DIR" ]; then
  echo "ArduinoJson headers not found: $ARDUINOJSON_DIR" >&2
  echo "Install them with: pio pkg install -e default --library \"bblanchon/ArduinoJson@7.4.2\"" >&2
  exit 1
fi

gcc -c "$ROOT_DIR/lib/md4c/md4c.c" -I"$ROOT_DIR/lib/md4c" -o "$BUILD_DIR/md4c.o"
gcc -c "$ROOT_DIR/lib/md4c/entity.c" -I"$ROOT_DIR/lib/md4c" -o "$BUILD_DIR/entity.o"

# Enable the web pokedex/pokemon party routes so host tests compile and exercise them.
g++ -std=c++20 -O2 -Wno-narrowing \
  -DENABLE_WEB_POKEDEX_PLUGIN=1 \
  -DENABLE_POKEMON_PARTY=1 \
  -I"$ROOT_DIR" \
  -I"$ROOT_DIR/test/mock" \
  -I"$ROOT_DIR/lib/FsHelpers" \
  -I"$ROOT_DIR/lib/Markdown" \
  -I"$ROOT_DIR/lib/md4c" \
  -I"$ROOT_DIR/lib/Serialization" \
  -I"$ROOT_DIR/include" \
  -I"$ROOT_DIR/src" \
  -I"$ARDUINOJSON_DIR" \
  "$ROOT_DIR/test/HostTests.cpp" \
  "$ROOT_DIR/lib/FsHelpers/FsHelpers.cpp" \
  "$ROOT_DIR/lib/Markdown/MarkdownParser.cpp" \
  "$ROOT_DIR/src/core/features/FeatureCatalog.cpp" \
  "$ROOT_DIR/src/features/pokemon_party/Registration.cpp" \
  "$ROOT_DIR/src/features/remote_keyboard_input/Registration.cpp" \
  "$ROOT_DIR/src/network/RemoteKeyboardSession.cpp" \
  "$ROOT_DIR/src/network/RecentBookJson.cpp" \
  "$ROOT_DIR/src/util/ForkDriftNavigation.cpp" \
  "$ROOT_DIR/src/util/BookProgressDataStore.cpp" \
  "$ROOT_DIR/src/util/InputValidation.cpp" \
  "$ROOT_DIR/src/util/PathUtils.cpp" \
  "$ROOT_DIR/src/util/PokemonBookDataStore.cpp" \
  "$ROOT_DIR/src/CrossPointSettings.cpp" \
  "$ROOT_DIR/test/mock/JsonSettingsIO.cpp" \
  "$BUILD_DIR/md4c.o" \
  "$BUILD_DIR/entity.o" \
  -o "$BUILD_DIR/HostTests"

"$BUILD_DIR/HostTests"
