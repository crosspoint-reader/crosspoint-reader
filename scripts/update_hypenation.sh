#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

process() {
  local lang="$1"

  mkdir -p "build"
  wget -O "build/$lang.bin" "https://github.com/typst/hypher/raw/refs/heads/main/tries/$lang.bin"

  python scripts/generate_hyphenation_trie.py \
    --input "build/$lang.bin" \
    --output "lib/Epub/Epub/hyphenation/generated/hyph-${lang}.trie.h"
}

process en
process fr
process de
process es
process ru
process it
