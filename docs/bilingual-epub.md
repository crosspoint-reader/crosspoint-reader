# Bilingual EPUB Format

CrossPoint renders bilingual EPUBs (original + translation in the same file) with an
in-reader toggle that swaps between **Both**, **Original only**, and **Translation only**
without leaving the page. This document defines the marker classes the parser looks for
and how a translation pipeline (or any other producer) should emit them.

## Marker classes

The parser (`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`) inspects the `class`
attribute of every element at parse time. Two class tokens are meaningful:

| Class token      | Meaning                                  |
|------------------|------------------------------------------|
| `cp-original`    | Source-language paragraph                |
| `cp-translation` | Target-language paragraph (Korean, etc.) |

Matching is **token-exact** (whitespace-separated). A paragraph marked
`class="chapter1 cp-original first"` is treated as original; `class="my-cp-original"`
is **not** matched (it would be invisible to the toggle and always rendered).

### Marker resolution is a hybrid (cp-* wins, xml:lang fallback)

As of v28 the toggle resolves a paragraph's role with a two-tier strategy so the
reader is interoperable with the wider bilingual-EPUB ecosystem, not just one
producer:

1. **Explicit cp-* tokens win first.** `class="cp-original"` / `class="cp-translation"`
   (token-exact, see `hasCssClass`). This is the unambiguous path used by the
   reference generator (`scripts/generate_bilingual_test_epub.py`).
2. **Standard `xml:lang` fallback.** When neither cp-* token is present, the
   parser reads the W3C/DAISY `xml:lang` attribute (or legacy `lang`). A
   paragraph whose **primary language subtag** matches the publication's
   `dc:language` (OPF) is the source; any other language is treated as the
   translation. This makes EPUBs from producers like
   [bookfere/Ebook-Translator-Calibre-Plugin](https://github.com/bookfere/Ebook-Translator-Calibre-Plugin)
   — which set `xml:lang` rather than our private classes — work without a
   conversion pass.
3. **Neither marker** → the paragraph is unaffected (toggle is a no-op on plain
   monolingual books).

This means a producer can pick whichever standard it already follows:

```html
<!-- Option A: explicit cp-* tokens (reference generator default) -->
<p class="cp-original">Hello world.</p>
<p class="cp-translation">안녕하세요, 세계.</p>

<!-- Option B: W3C xml:lang only (bookfere/Standard Ebooks style) -->
<p xml:lang="en">Hello world.</p>
<p xml:lang="ko">안녕하세요, 세계.</p>

<!-- Option C: both — cp-* wins, xml:lang adds screen-reader accessibility -->
<p class="cp-original" xml:lang="en">Hello world.</p>
<p class="cp-translation" xml:lang="ko">안녕하세요, 세계.</p>
```

Primary-subtag matching (`en-US` → `en`) keeps regional variants from tripping
the comparison.

#### Documented edge case

A foreign-language quotation inside a monolingual book (e.g. a French passage
in an English novel, marked `xml:lang="fr"`) will be classified as a translation
and hidden in Translation-only mode. cp-* override avoids this for our pipeline;
books that rely on the xml:lang path should set `dc:language` to the work's true
primary language so the source/translation split is unambiguous.

## Toggle behavior

The reader cycles the `Bilingual View Mode` setting on long-press Confirm when the
`Long-press Menu` setting (Settings → Controls → Long-press Menu) is bound to
**Bilingual Toggle**. The cycle is:

```
Both (0)  →  Original only (1)  →  Translation only (2)  →  Both (0)  →  …
```

- **Both (default)** — every paragraph renders normally; no filtering. EPUBs without
  the marker classes are unaffected by the other two modes (filtered paragraphs are
  just absent, so the modes are no-ops on plain books).
- **Original only** — elements carrying `cp-translation` are dropped during parsing
  (treated as `display:none` before block creation).
- **Translation only** — elements carrying `cp-original` are dropped.

The mode is stored in the section cache header (`SECTION_FILE_VERSION = 28`), so
switching modes invalidates the per-section cache and the current chapter re-parses
on the next render. Section files written by older firmwares (v27 and below) are
discarded automatically on first open after upgrade.

## Required HTML shape

The minimum viable bilingual EPUB alternates the two markers paragraph-by-paragraph:

```html
<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <body>
    <p class="cp-original">It is a truth universally acknowledged, ...</p>
    <p class="cp-translation">누구나 인정하는 보편적 진리이다, ...</p>
    <p class="cp-original">However little known the feelings or views ...</p>
    <p class="cp-translation">그런 남자의 감정이나 견해가 ...</p>
  </body>
</html>
```

Ordering within a paragraph pair does not matter — both modes use class match only,
not document order. Wrapping elements (e.g. `<div class="chapter">` around many
paragraphs) is fine; the override is applied at the `<p>` level.

Other class tokens on the same element are allowed (`class="dialogue cp-original"`)
as long as the exact `cp-original` / `cp-translation` token appears.

## What is intentionally not supported

- **Two-file dual-EPUB schemes** (separate `book.en.epub` + `book.ko.epub` linked by
  anchor). The ESP32-C3 cannot hold two parsed sections in RAM simultaneously. The
  single-file bilingual approach keeps one parse tree in memory and lets CSS display
  rules do the filtering.
- **Page-level (not paragraph-level) mapping**. Modes hide paragraphs, not pages,
  because the page layout depends on which paragraphs are visible. Switching to
  Original-only reflows the chapter and may change the page count.
- **Inline (sentence-level) bilingual markup**. Only block-level `<p>` filtering is
  wired. If a translation tool emits `<span class="cp-original">` inside a paragraph,
  the toggle has no effect on it.

## Producer-side configuration

When generating EPUBs from a translation pipeline, configure the tool so its
output emits the marker classes above:

- **DocuTranslate** (recommended): use `insert_mode="append"` (or `"prepend"`) with
  `original_class="cp-original"` and `translation_class="cp-translation"`. If the
  tool's options do not allow custom class names, post-process its output with a
  single regex pass to rename whatever classes it emits.
- **TranslateBooksWithLLMs (TBL)**: enable bilingual mode, then run a post-processor
  that adds the `cp-*` class tokens to each half of every emitted pair.
- **Ebook-Translator (Calibre plugin)**: in bilingual output mode, override the
  paragraph templates to use `cp-original` / `cp-translation`.

A reference generator that produces a valid minimal bilingual EPUB with these markers
lives at `scripts/generate_bilingual_test_epub.py`. Run it locally and push the result
to the device SD card to smoke-test the toggle before wiring the full pipeline:

```bash
python3 scripts/generate_bilingual_test_epub.py
# → test/epubs/bilingual-sample.epub
```

## Testing the toggle end-to-end

1. Flash a firmware built from `feature/bilingual-toggle` (or a release that contains
   this feature) to the Xteink X3 / X4.
2. Copy `bilingual-sample.epub` to the SD card and open it.
3. Settings → Controls → Long-press Menu → choose **Bilingual Toggle**.
4. Back in the reader, long-press Confirm (~0.4 s). The chapter should re-parse and:
   - Both modes: every paragraph visible.
   - Original only: only the English paragraphs.
   - Translation only: only the Korean paragraphs.
5. Long-press Confirm again to advance to the next mode.

If nothing changes on toggle, verify the EPUB actually carries the class tokens:

```bash
unzip -p book.epub OEBPS/chapter1.xhtml | grep -o 'class="cp-[a-z]*"' | sort -u
# Expected:
#   class="cp-original"
#   class="cp-translation"
```
