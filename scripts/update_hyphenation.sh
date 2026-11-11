#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HYPHER_DIR="$(cd "${1:?Pass a checkout of typst/hypher at d81cc506416bffef2a75ee2dc969d4b41bc36613}" && pwd)"
HYPHER_REVISION=d81cc506416bffef2a75ee2dc969d4b41bc36613
test "$(git -C "$HYPHER_DIR" rev-parse HEAD)" = "$HYPHER_REVISION"

cd "$ROOT_DIR"

process() {
  local lang="$1"

  python scripts/generate_hyphenation_trie.py \
    --input "$HYPHER_DIR/tries/$lang.bin" \
    --output "lib/Epub/Epub/hyphenation/generated/hyph-${lang}.trie.h"
}

process en
process fr
process de
process es
process ru
process it
process uk
process pl
process pt
process sv
process fi
