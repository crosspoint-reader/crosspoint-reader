# PR 3: feat: Configurable Cover Skipping

## Goal
- Add a Reader setting that skips cover sections on fresh book entry.
- Never override saved progress, sync positions, or explicit navigation.

## Scope
- Reader setting `skipCoverOnBookEntry`, JSON key, and translated label.
- EPUB metadata/cache support for cover-page hrefs.
- Fresh-entry spine selection for text/start references, cover-page spine 0, and small cover wrappers.
- Saved `spine=0,page=0` progress is treated as cover-start progress when the setting is enabled.

## Start Metadata Behavior
- OPF `guide` references with `type="text"` are still respected.
- OPF `guide` references with `type="start"` are also treated as start/text references when no `text` reference has already been found.
- On fresh entry, a resolved text/start reference wins before cover-page and cover-wrapper heuristics, as long as it points after spine 0. This preserves publisher-provided "start reading here" metadata for books that explicitly skip front matter.
- If the text/start reference is missing, unresolved, or points at spine 0, the cover-skip heuristics may still skip a detected cover page/wrapper at spine 0 to spine 1.
- This logic only runs for fresh entry. Saved progress, KOReader Sync remote positions, and explicit navigation paths do not use the cover-skip decision.

## Verification
- `python test/cover_skip_static_contracts.py`
- `./bin/clang-format-fix`
- `pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high`
- `pio run -e default`

---

### AI Usage

While CrossPoint doesn't have restrictions on AI tools in contributing, please be transparent about their usage as it 
helps set the right context for reviewers.

Did you use AI tools to help write this code? _**YES**_
