#!/usr/bin/env bash
# Pull upstream Biscuit changes into the vendored biscuit/ subtree.
#
# biscuit/ is vendored with `git subtree --squash`; this re-merges upstream
# while preserving our local [BOOTSWITCH-PATCH] edits (biscuit/LOCAL_PATCHES.md
# is the crib sheet for resolving conflicts on the files we modified).
set -euo pipefail

BISCUIT_URL="${BISCUIT_URL:-https://github.com/yattsu/biscuit.git}"
BRANCH="${1:-master}"

cd "$(git rev-parse --show-toplevel)"

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Working tree is dirty — commit or stash first." >&2
  exit 1
fi

if [[ ! -f biscuit/platformio.ini ]]; then
  echo "biscuit/ not vendored yet; run the initial import instead:" >&2
  echo "  git subtree add --prefix=biscuit ${BISCUIT_URL} ${BRANCH} --squash" >&2
  exit 1
fi

git subtree pull --prefix=biscuit "${BISCUIT_URL}" "${BRANCH}" --squash

echo
echo "== post-sync checklist (see biscuit/LOCAL_PATCHES.md) =="
echo "1. Every local patch must still carry its marker:"
grep -Rn "BOOTSWITCH-PATCH" biscuit/ || echo "   !! No BOOTSWITCH-PATCH markers found — the merge dropped them; reapply from biscuit/LOCAL_PATCHES.md"
echo "2. biscuit/platformio.ini must still point at the shared table and flags:"
grep -n "partitions" biscuit/platformio.ini || true
grep -n "DUAL_OS_LOCK" biscuit/platformio.ini || true
grep -n "BootSwitch" biscuit/platformio.ini || true
echo "3. Rebuild both firmwares and watch for 'does not fit' (slot overflow):"
echo "     make -C tools all"
echo "4. Re-test the swap on hardware in both directions, then commit the merge."
