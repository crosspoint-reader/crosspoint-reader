# PR 1: fix: Incremental Indexing Redraw Cleanup

## Goal
- Keep normal EPUB first-page loads free of pre-page indexing popups and redraws.
- Stop background and prewarm indexing state changes from repainting the current page.
- Improve genuine cold-start metadata indexing for medium-size EPUBs.
- Preserve responsive sleep/manual input behavior while indexing is active.

## Scope
- `EpubReaderActivity` redraw scheduling around background/prewarm indexing.
- Reader-menu book cache deletion removes `progress.bin` instead of rewriting it.
- Fresh EPUB entry shows the Indexing popup before missing `book.bin`/progress work, and fresh-entry section creation reuses that feedback without adding a duplicate redraw.
- Medium-size EPUBs now use the fast OPF item index, TOC spine lookup index, and batch ZIP size lookup instead of waiting for very large spine counts.
- `book.bin` construction logs sub-stage timings so cold-start gains can be measured on-device.
- Manual sleep now drains the queued SleepActivity transition directly instead of running one more outgoing reader loop first, and foreground section pumps stop between batches when sleep is pending.
- EPUB progress percent calculations now share the same displayed-page basis as the status bar, avoiding a one-page lag in the reader menu, percent selector, and screenshot metadata.
- Incremental anchor cache reads now validate serialized anchor key length before allocating, so a corrupt `.anc` file cannot request an unbounded string allocation.
- Static incremental contracts that protect the redraw and indexing-popup behavior.

## Explicitly Not In This PR
- Image-only section rendering and SVG-wrapped raster image parsing are covered by PR 2.
- Configurable cover skipping and `text/start` fresh-entry behavior are covered by PR 3.

## Verification
- `python test/incremental_section/StaticIncrementalContracts.py`
- `./bin/clang-format-fix`
- Direct host tests:
  - `IncrementalSectionTypesTest`
  - `EpubIndexingPolicyTest`
  - `ReaderProgressPolicyTest`
  - `StatusPageInfoTest`
- `git diff --check`
- `test/run_incremental_section_tests.sh`
- `pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high`
- `pio run --jobs 1`
