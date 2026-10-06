#!/bin/bash
# Pocket Library for CrossPoint — fetch the card-building tools onto a Mac.
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash get-tools.sh
#
# Puts cardbuilder.py, library.toml, zimindex and zimcat (and the optional
# webpack tool for Pet First Aid) in ~/PocketLib/cardbuilder, each checked
# against the checksum published beside it. Run it again to update them; an
# existing library.toml is kept (the new one is saved as library.toml.new).

set -euo pipefail

REPO="noah-pi/pocket-library"
RELEASE="${POCKETLIB_RELEASE:-dev}"
DIR="${POCKETLIB_HOME:-$HOME/PocketLib}/cardbuilder"

say() { printf '%s\n' "$*"; }
die() {
  printf '\nStopped: %s\n' "$*" >&2
  exit 1
}
sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

# The card builder reads library.toml with tomllib (Python 3.11+); macOS's
# own python3 is 3.9.
PY=""
for c in python3.14 python3.13 python3.12 python3.11 python3; do
  if command -v "$c" >/dev/null 2>&1 &&
    "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
    PY="$c"
    break
  fi
done
[ -n "$PY" ] || die "the card builder needs Python 3.11 or newer. Install it from https://www.python.org/downloads/ (the macOS installer, a few minutes), then run this again."

case "$(uname -s)" in
  Darwin) ;;
  *) say "Note: zimindex and zimcat from the release are built for macOS. On Linux, build them from the repository (see tools/cardbuilder/README.md)." ;;
esac

mkdir -p "$DIR"
base="https://github.com/$REPO/releases/download/$RELEASE"
for name in cardbuilder.py library.toml zimindex zimcat webpack.py zimwrite.py pets.toml; do
  dest="$DIR/$name"
  if [ "$name" = library.toml ] && [ -f "$dest" ]; then dest="$DIR/library.toml.new"; fi
  say "Downloading $name ..."
  curl -fsSL --retry 3 -o "$dest" "$base/$name" || die "download failed: $base/$name"
  curl -fsSL --retry 3 -o "$dest.sha256" "$base/$name.sha256" || die "checksum download failed for $name"
  [ "$(awk '{print $1}' "$dest.sha256")" = "$(sha256 "$dest")" ] || die "$name does not match its published checksum; run this again"
  rm -f "$dest.sha256"
done
chmod +x "$DIR/zimindex" "$DIR/zimcat"

say ""
say "Tools are in $DIR (checksums OK)."
if [ -f "$DIR/library.toml.new" ]; then
  say "Your library.toml was kept; the latest one is library.toml.new beside it."
fi
say ""
say "Next:"
say "  cd \"$DIR\""
say "  $PY cardbuilder.py plan --card /Volumes/POCKETLIB"
