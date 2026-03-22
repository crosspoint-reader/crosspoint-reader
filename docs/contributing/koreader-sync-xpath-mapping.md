# KOReader Sync XPath Mapping

This note documents how CrossPoint maps reading positions to and from KOReader sync payloads.

## Problem

CrossPoint internally stores position as:

- `spineIndex` (chapter index, 0-based)
- `pageNumber` + `totalPages`

KOReader sync payload stores:

- `progress` (XPath-like location)
- `percentage` (overall progress)

A direct 1:1 mapping is not guaranteed because page layout differs between engines/devices.

## DocFragment Index Convention

KOReader uses **1-based** XPath predicates throughout, following standard XPath conventions.
The first EPUB spine item is `DocFragment[1]`, the second is `DocFragment[2]`, and so on.

CrossPoint stores spine items as 0-based indices internally. The conversion is:

- **Generating XPath (to KOReader):** `DocFragment[spineIndex + 1]`
- **Parsing XPath (from KOReader):** `spineIndex = DocFragment[N] - 1`

Reference: [koreader/koreader#11585](https://github.com/koreader/koreader/issues/11585) confirms this
via a KOReader contributor mapping spine items to DocFragment numbers.

## Current Strategy

### CrossPoint -> KOReader

Implemented in `ProgressMapper::toKOReader`.

1. Compute overall `percentage` from chapter/page.
2. If a paragraph index is available from the section cache LUT (`CrossPointPosition::hasParagraphIndex`),
   generate an XPath directly: `/body/DocFragment[spineIndex + 1]/body/p[paragraphIndex]`.
3. Otherwise, attempt byte-offset estimation via `ChapterXPathIndexer::findXPathForProgress`.
4. If XPath extraction fails, fallback to synthetic chapter path:
   - `/body/DocFragment[spineIndex + 1]/body`

### KOReader -> CrossPoint

Implemented in `ProgressMapper::toCrossPoint`.

1. Attempt to parse `DocFragment[N]` from incoming XPath; convert N to 0-based `spineIndex = N - 1`.
2. If valid, attempt XPath-to-offset mapping via `ChapterXPathIndexer::findProgressForXPath`.
3. Extract paragraph index from XPath via `ChapterXPathIndexer::tryExtractParagraphIndexFromXPath`
   (e.g. `/body/DocFragment[7]/body/p[685]/text().96` → `paragraphIndex = 685`).
4. Convert resolved intra-spine progress to page estimate.
5. If XPath path is invalid/unresolvable, fallback to percentage-based chapter/page estimation.

When a paragraph index is available, `EpubReaderActivity` refines the page estimate using
the section cache's per-page paragraph LUT (`Section::getPageForParagraphIndex`). This finds
the first page whose recorded paragraph index is >= the target, giving a more accurate
landing position than byte-offset-based estimation alone.

## ChapterXPathIndexer Design

The module reparses **one spine XHTML** on demand using Expat and builds temporary anchors:

Source-of-truth note: XPath anchors are built from the original EPUB spine XHTML bytes (zip item contents), not from CrossPoint's distilled section render cache. This is intentional to preserve KOReader XPath compatibility.

- anchor: `<xpath, textOffset>`
- `textOffset` counts non-whitespace bytes
- When multiple anchors exist for the same path, the one with the **smallest** textOffset is used
  (start of element), not the latest periodic anchor.

Forward lookup (CrossPoint → XPath): uses `upper_bound` to find the last anchor at or before the
target text offset, ensuring the returned XPath corresponds to the element the user is currently
inside rather than the next element.

Matching for reverse lookup:

1. exact path match — reported as `exact=yes`
2. index-insensitive path match (`div[2]` vs `div[3]` tolerated) — reported as `exact=no`
3. ancestor fallback — reported as `exact=no`

If no match is found, caller must fallback to percentage.

## Memory / Safety Constraints (ESP32-C3)

The implementation intentionally avoids full DOM storage.

- Parse one chapter only.
- Keep anchors in transient vectors only for duration of call.
- Free XML parser and chapter byte buffer on all success/failure paths.
- No persistent cache structures are introduced by this module.

## Paragraph Index LUT

The section cache stores a per-page paragraph index LUT built during page layout
(`ChapterHtmlSlimParser`). Each entry records the 1-based `<p>` sibling index
(direct children of `<body>`, matching XPath convention) at the time each page was completed.

This enables two lookups without reparsing:

- **XPath → page** (`Section::getPageForParagraphIndex`): finds the first page where the
  recorded paragraph index >= target. Used when applying remote KOReader progress.
- **Page → XPath** (`Section::getParagraphIndexForPage`): returns the paragraph index for
  a given page. Used when uploading local progress to KOReader.

The paragraph counter in `ChapterHtmlSlimParser` counts **all** `<p>` elements at body-child
level, including `display:none` elements. This matches `ChapterXPathIndexer` and crengine's
standard XPath same-name sibling counting.

## Known Limitations

- Page number on reverse mapping is still an estimate (renderer differences).
  The paragraph LUT refines this but cannot guarantee exact page matching.
- XPath mapping intentionally uses original spine XHTML while pagination comes from distilled renderer output, so minor roundtrip page drift is expected.
- Image-only/low-text chapters may yield coarse anchors.
- Extremely malformed XHTML can force fallback behavior.

## Operational Logging

`ProgressMapper` logs mapping source in reverse direction:

- `xpath` when XPath mapping path was used
- `percentage` when fallback path was used

It also logs exactness (`exact=yes/no`) for XPath matches. Note that `exact=yes` is only set for
a full path match with correct indices; index-insensitive and ancestor matches always log `exact=no`.
