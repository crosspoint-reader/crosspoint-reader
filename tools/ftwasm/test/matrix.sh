#!/usr/bin/env bash
# Parity of every built wasm variant against one Python reference dir.
#   test/matrix.sh [ref-dir] [sets]      (defaults: out/ref-wheel, core,literata,wide,otf)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="${1:-out/ref-wheel}"
SETS="${2:-core,literata,wide,otf}"
shopt -s nullglob
dists=("$HERE"/dist/ft*/)
[[ -f "$HERE/dist/ftwasm.mjs" ]] && dists+=("$HERE/dist/")
for d in "${dists[@]}"; do
  d="${d%/}"; v="$(basename "$d")"
  node "$HERE/test/parity.mjs" --dist "$d" --ref "$REF" --sets "$SETS" \
       --json "$HERE/out/matrix-$v-$(basename "$REF").json" --show 0 > "$HERE/out/matrix-$v-$(basename "$REF").log" 2>&1 || true
  printf '%-18s %s\n' "$v" "$(grep '^TOTAL' "$HERE/out/matrix-$v-$(basename "$REF").log")"
done
