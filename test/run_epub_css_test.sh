#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/epub_css"
BINARY="$BUILD_DIR/CssParserTest"
PLATFORMIO_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
ARDUINO_FRAMEWORK_DIR="$PLATFORMIO_DIR/packages/framework-arduinoespressif32"

PIO_CMD=""
if command -v pio >/dev/null 2>&1; then
  PIO_CMD="pio"
elif command -v platformio >/dev/null 2>&1; then
  PIO_CMD="platformio"
fi

if [ ! -d "$PLATFORMIO_DIR" ] && [ -n "$PIO_CMD" ]; then
  PLATFORMIO_DIR="$($PIO_CMD settings get home_dir 2>/dev/null || true)"
  if [ -n "$PLATFORMIO_DIR" ] && [ -d "$PLATFORMIO_DIR" ]; then
    ARDUINO_FRAMEWORK_DIR="$PLATFORMIO_DIR/packages/framework-arduinoespressif32"
    echo "Using PLATFORMIO_DIR from $PIO_CMD: $PLATFORMIO_DIR"
  fi
fi

if [ ! -d "$PLATFORMIO_DIR" ]; then
  echo "SKIP: PLATFORMIO_DIR does not exist and PlatformIO could not be located; skipping EPUB CSS test." >&2
  exit 0
fi

if [ ! -d "$ARDUINO_FRAMEWORK_DIR" ]; then
  echo "SKIP: ARDUINO_FRAMEWORK_DIR does not exist: $ARDUINO_FRAMEWORK_DIR; skipping EPUB CSS test." >&2
  exit 0
fi

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/epub_css/CssParserTest.cpp"
  "$ROOT_DIR/lib/Epub/Epub/css/CssParser.cpp"
  "$ROOT_DIR/lib/hal/HalStorage.cpp"
  "$ROOT_DIR/lib/Logging/Logging.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -fno-exceptions
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DDESTRUCTOR_CLOSES_FILE=1
  -I"$ROOT_DIR/test/shims"
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/hal"
  -I"$ROOT_DIR/lib/Logging"
  -I"$ARDUINO_FRAMEWORK_DIR/cores/esp32"
  -I"$ARDUINO_FRAMEWORK_DIR/variants/esp32c3"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
