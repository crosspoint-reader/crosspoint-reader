#!/bin/bash
# Pocket Library for CrossPoint — put the firmware on an Xteink X4 Pro.
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# For a Mac (also works on Linux). Run it from Terminal with the reader
# plugged in by USB-C and awake:
#
#   bash flash.sh              install the latest Pocket Library firmware
#   bash flash.sh FILE.bin     install a firmware file you already have
#   bash flash.sh backup       copy the reader's whole memory to a file
#   bash flash.sh restore FILE put a backup made by "backup" back
#   bash flash.sh stock        install plain CrossPoint 1.6.5 (to go back)
#   bash flash.sh log          show the reader's log (Control-] to stop)
#
# The first install also makes a full backup, so the reader can always be put
# back exactly as it was. X4 Pro only: never use this on an X4, X3 or other
# ESP32-C3 reader.

set -euo pipefail

REPO="noah-pi/pocket-library"
RELEASE="${POCKETLIB_RELEASE:-dev}"
HOME_DIR="${POCKETLIB_HOME:-$HOME/PocketLib}"
BACKUPS="$HOME_DIR/backups"
DOWNLOADS="$HOME_DIR/downloads"
BAUD=921600
ESPTOOL=""
PORT=""

say() { printf '%s\n' "$*"; }
die() {
  printf '\nStopped: %s\n' "$*" >&2
  exit 1
}
# Questions read the keyboard even when the script itself arrives on a pipe.
ask() {
  local reply=""
  read -r -p "$1 [y/N] " reply </dev/tty || true
  [ "$reply" = y ] || [ "$reply" = Y ] || [ "$reply" = yes ]
}

sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

# A Python 3.11 or newer. macOS's own python3 is 3.9, too old for esptool 5.
pick_python() {
  local c
  for c in python3.14 python3.13 python3.12 python3.11 python3; do
    if command -v "$c" >/dev/null 2>&1 &&
      "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
      command -v "$c"
      return
    fi
  done
  die "this needs Python 3.11 or newer. Install it from https://www.python.org/downloads/ (the macOS installer, a few minutes), then run this again."
}

# esptool, Espressif's flashing tool, in a private Python environment: macOS's
# Homebrew Python refuses a plain "pip install", and "brew install esptool"
# compiles for hours on older macOS.
find_esptool() {
  if [ -x "$HOME_DIR/esptool-env/bin/esptool" ]; then
    ESPTOOL="$HOME_DIR/esptool-env/bin/esptool"
    return
  fi
  local py
  py=$(pick_python)
  say "Setting up esptool (once, about a minute) in $HOME_DIR/esptool-env ..."
  mkdir -p "$HOME_DIR"
  "$py" -m venv "$HOME_DIR/esptool-env" || die "could not make a Python environment"
  "$HOME_DIR/esptool-env/bin/python" -m pip install --quiet --upgrade pip esptool || die "could not install esptool"
  ESPTOOL="$HOME_DIR/esptool-env/bin/esptool"
}

esptool_python() { printf '%s/python' "$(dirname "$ESPTOOL")"; }

find_port() {
  local ports count
  ports=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true)
  if [ -z "$ports" ]; then
    die "no reader found. Plug it in with a USB-C cable that carries data (some only charge), press the power button to wake it, and run this again."
  fi
  count=$(printf '%s\n' "$ports" | wc -l | tr -d ' ')
  if [ "$count" != 1 ]; then
    die "more than one USB device answers ($(printf '%s ' $ports)). Unplug the others and run this again."
  fi
  PORT="$ports"
  say "Reader on $PORT"
}

# Steps before the last leave the chip waiting for the next command
# (no-reset); the last one restarts it.
run_esptool() {
  "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud "$BAUD" "$@"
}

# Downloads NAME from the release into $DOWNLOADS and checks it against the
# NAME.sha256 published beside it. Sets FILE.
FILE=""
fetch() {
  local name="$1" base want got
  base="https://github.com/$REPO/releases/download/$RELEASE"
  mkdir -p "$DOWNLOADS"
  say "Downloading $name from the \"$RELEASE\" release ..."
  curl -fL --retry 3 -o "$DOWNLOADS/$name" "$base/$name" || die "download failed: $base/$name"
  curl -fsL --retry 3 -o "$DOWNLOADS/$name.sha256" "$base/$name.sha256" || die "checksum download failed"
  want=$(awk '{print $1}' "$DOWNLOADS/$name.sha256")
  got=$(sha256 "$DOWNLOADS/$name")
  [ "$want" = "$got" ] || die "$name does not match its published checksum. Run this again to download it afresh."
  say "  checksum OK"
  FILE="$DOWNLOADS/$name"
}

backup() {
  local out
  mkdir -p "$BACKUPS"
  out="$BACKUPS/x4pro-backup-$(date +%Y%m%d-%H%M%S).bin"
  say ""
  say "Backing up the reader's whole memory (16 MB, about 3 minutes) to"
  say "  $out"
  run_esptool --after no-reset read-flash 0 ALL "$out" || die "backup failed (is the reader awake?)"
  [ "$(wc -c <"$out" | tr -d ' ')" -ge 16000000 ] || die "the backup came out too small; nothing was changed"
  say "  backup done. Keep this file: \"bash flash.sh restore\" puts it back."
}

# Pocket Library replaces CrossPoint's app slot, so the reader must already
# have CrossPoint's layout: app0 at 0x10000 and the boot-choice record at
# 0xe000. A reader still on Xteink's own software has another layout.
check_layout() {
  local table
  table=$(mktemp)
  run_esptool --after no-reset read-flash 0x8000 0x1000 "$table" >/dev/null || {
    rm -f "$table"
    die "could not read the reader (press the power button to wake it, then run this again)"
  }
  if ! "$(esptool_python)" - "$table" <<'PY'; then
import struct, sys
data = open(sys.argv[1], "rb").read()
parts = {}
for i in range(0, len(data) - 31, 32):
    entry = data[i:i + 32]
    if entry[:2] != b"\xaa\x50":
        break
    offset, size = struct.unpack("<II", entry[4:12])
    label = entry[12:28].split(b"\0")[0].decode("ascii", "replace")
    parts[label] = (offset, size)
ok = parts.get("app0", (0, 0))[0] == 0x10000 and parts.get("app0", (0, 0))[1] >= 0x600000 \
    and parts.get("otadata", (0, 0))[0] == 0xE000
sys.exit(0 if ok else 1)
PY
    rm -f "$table"
    say ""
    say "This reader doesn't have CrossPoint on it yet (it still runs Xteink's own"
    say "software), so Pocket Library can't go on directly. Do this once:"
    say "  1. Open https://crosspointreader.com in Chrome and choose Flash tools."
    say "  2. Pick Xteink X4 Pro and install the official CrossPoint."
    say "  3. When the reader starts in CrossPoint, run this script again."
    die "CrossPoint needed first"
  fi
  rm -f "$table"
}

install() {
  local firmware="$1"
  find_esptool
  find_port
  if ! ls "$BACKUPS"/x4pro-backup-*.bin >/dev/null 2>&1; then
    say ""
    say "First time on this Mac: a full backup of the reader comes first, so it can"
    say "always be put back exactly as it is now."
    if ask "Make the backup now? (recommended)"; then
      backup
    else
      say "  skipped the backup."
    fi
  fi
  check_layout
  if [ -z "$firmware" ]; then
    fetch pocketlib-x4pro.bin
    firmware="$FILE"
  fi
  [ -f "$firmware" ] || die "no such file: $firmware"
  say ""
  say "Ready to install $(basename "$firmware") ($(wc -c <"$firmware" | tr -d ' ') bytes)."
  say "Your books, settings and the card are not touched."
  ask "Install it now?" || die "nothing was changed"
  run_esptool --after no-reset write-flash 0x10000 "$firmware" || die "writing failed. Wake the reader and run this again; it starts over safely."
  # Clear the record of which app slot to start, so the reader starts the one
  # just written (an earlier update may have pointed it at the other slot).
  run_esptool erase-region 0xe000 0x2000 || die "the last step failed. Run this again."
  say ""
  say "Done. The reader restarts by itself (press the power button if it doesn't)."
  say "Check Settings > About: the version ends in -pocketlib-<code>."
}

restore() {
  local file="${1:-}"
  [ -n "$file" ] || file=$(ls -t "$BACKUPS"/x4pro-backup-*.bin 2>/dev/null | head -1 || true)
  [ -n "$file" ] && [ -f "$file" ] || die "say which backup: bash flash.sh restore FILE"
  find_esptool
  find_port
  say "This puts the whole reader back to $file."
  ask "Restore it now?" || die "nothing was changed"
  run_esptool write-flash 0 "$file" || die "restore failed. Wake the reader and run this again."
  say "Restored. The reader restarts by itself."
}

show_log() {
  find_esptool
  find_port
  say "Showing the reader's log. Use it, then press Control-] to stop."
  # RTS and DTR held low: opening the port must not restart the reader.
  "$(esptool_python)" -m serial.tools.miniterm --rts 0 --dtr 0 "$PORT" 115200
}

case "${1:-install}" in
  install) install "" ;;
  backup)
    find_esptool
    find_port
    backup
    run_esptool --after hard-reset chip-id >/dev/null || true
    ;;
  restore) restore "${2:-}" ;;
  stock)
    fetch stock-1.6.5-x4pro.bin
    install "$FILE"
    ;;
  log) show_log ;;
  -h | --help | help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//' ;;
  *.bin) install "$1" ;;
  *) die "unknown command: $1 (try: bash flash.sh help)" ;;
esac
