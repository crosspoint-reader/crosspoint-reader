#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")/.."

PIO="${PIO:-pio}"
if [ -x ".venv/bin/pio" ]; then
  PIO=".venv/bin/pio"
fi

mkdir -p emu_sd/books
"$PIO" run -e emulator
CROSSPOINT_EMU_SD="$PWD/emu_sd" .pio/build/emulator/program
