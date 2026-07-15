#!/usr/bin/env bash
# personal-branch helper: remove every translation except English to shrink
# flash (~250 KB for 29 languages). Run after each `git merge develop`, which
# re-adds new upstream translation files and raises modify/delete conflicts
# on the already-stripped ones. Regenerates the i18n tables afterwards.
set -euo pipefail

cd "$(dirname "$0")/.."

# Resolve merge conflicts on stripped files by keeping them deleted.
git diff --name-only --diff-filter=U -- 'lib/I18n/translations/*.yaml' | while read -r f; do
  [ "$(basename "$f")" = "english.yaml" ] && continue
  git rm -q "$f"
  echo "resolved (deleted): $f"
done

# Remove any non-English translations (new upstream files come back silently).
for f in lib/I18n/translations/*.yaml; do
  [ "$(basename "$f")" = "english.yaml" ] && continue
  git rm -q "$f" 2>/dev/null || rm -f "$f"
  echo "removed: $f"
done

python scripts/gen_i18n.py lib/I18n/translations lib/I18n/ | tail -3
echo "Done. Review with 'git status', then commit."
