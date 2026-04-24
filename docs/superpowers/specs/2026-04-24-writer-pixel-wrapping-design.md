# Writer Pixel Wrapping Design

## Summary

Stage 2 should improve Writer's wrapped-line layout so the visible text uses the actual rendered width of the X4 screen rather than an estimated character count.

The current `WriterWrappedLayout` keeps the right editor-facing shape: each line carries its displayed text plus `startOffset` and `endOffset` byte ranges for cursor mapping. The weakness is that line breaks are based on an approximate number of codepoints. This makes narrow and wide glyphs wrap at the same point, so the editor feels narrower and less polished than the reader UI.

This stage should keep the cursor-aware layout model and improve the wrapping decision.

## Goals

- Wrap Writer text using rendered pixel width.
- Preserve `WriterWrappedLayout::Line` offsets so cursor movement keeps working.
- Reuse the existing `GfxRenderer::wrappedText()` behavior and measuring approach where it fits.
- Keep the change small enough to test in the simulator.
- Avoid changing storage, draft selection, Bluetooth input, or insert-at-caret editing in this stage.

## Non-Goals

- No hyphenation.
- No full text justification.
- No reader pagination reuse.
- No large-document cache/window implementation.
- No new i18n work.
- No multi-draft workflow.

## Existing Reference

`GfxRenderer::wrappedText()` is the best local reference for screen-aware wrapping. It already:

- measures candidate lines with `getTextWidth(...)`
- builds lines by adding words until the next word no longer fits
- uses `truncatedText(...)` for display-only overflow cases
- handles font id and font family style

Writer should borrow the successful parts of that flow, especially rendered-width measurement and word-based line growth.

Writer should not call `wrappedText()` directly for the main editor layout because `wrappedText()` returns only strings. Writer also needs byte offsets for cursor positioning, vertical movement, and future insert-at-caret editing.

Writer should also avoid `wrappedText()`'s display-only truncation behavior. In a text editor, an overlong word should hard-wrap at a UTF-8-safe boundary rather than disappear behind an ellipsis.

## Proposed Architecture

`WriterWrappedLayout` remains the Writer-specific layout helper.

Its output stays:

```cpp
struct Line {
  std::string text;
  size_t startOffset;
  size_t endOffset;
};
```

The wrapping API should evolve from character-count wrapping toward pixel-width wrapping. A likely first shape is:

```cpp
static std::vector<Line> wrap(const std::string& renderedText,
                              int fontId,
                              int maxWidth,
                              const GfxRenderer& renderer);
```

If passing the renderer into the helper feels too coupled during implementation, an alternative is to pass a small measuring callback. The important boundary is that `WriterWrappedLayout` owns line/offset decisions, while `GfxRenderer` remains the source of truth for rendered text width.

`WriterActivity` should stop estimating wrap columns for this path and pass the actual content width instead.

## Wrapping Rules

For each newline-delimited paragraph:

- Preserve empty paragraphs as empty lines.
- Grow a line by UTF-8-safe codepoint ranges.
- Prefer breaking at spaces when the candidate line exceeds `maxWidth`.
- Do not include the breaking space at the end of the emitted line.
- Skip the breaking space at the start of the next line.
- If a single word is wider than `maxWidth`, hard-break it at the last UTF-8-safe position that fits.
- If even one codepoint is wider than `maxWidth`, emit that codepoint as its own line so the algorithm always makes progress.
- Never insert ellipses or discard editor text during wrapping.

This preserves editor correctness while matching the reader UI's visible width more closely.

## Staged Implementation

### Stage 2A: Pixel-Width Layout

Change `WriterWrappedLayout` so line breaks are decided with rendered pixel width.

Expected code changes:

- adjust `WriterWrappedLayout::wrap(...)` to accept font/width/measurement context
- use `renderer.getTextWidth(...)` to test candidate line widths
- keep existing UTF-8-safe offset progression
- update `WriterActivity::render()` and movement helpers to call the new layout API
- remove or bypass `estimateWrapColumns(...)` for wrapping if it becomes unused

Expected tests:

- short text fits on one line
- words wrap before exceeding max width
- spaces are used as preferred break points
- long words hard-wrap without ellipsis
- UTF-8 text is not split inside a codepoint
- empty lines remain visible as empty lines

### Stage 2B: Caret Measurement Consistency

After pixel wrapping works, check that caret x-position uses the same font and width assumptions as the wrapper.

The current caret measurement uses rendered text width over substrings. That is likely still correct, but this stage should verify that horizontal and vertical cursor movement still feels stable after line breaks move.

### Stage 2C: Render-Path Churn

Address the issue that `render()` and `renderFooter()` can rebuild or rescan rendered text separately.

Possible fixes:

- pass `renderedText` into `renderFooter()`
- pass a precomputed word count into `renderFooter()`
- compute render-local footer state beside wrapped layout state

This should stay separate from Stage 2A unless it falls out naturally.

### Stage 2D: Layout Caching Decision

Only after pixel wrapping works, decide whether wrapped lines need caching.

If simulator and hardware performance remain acceptable, do not add caching yet. If wrapping becomes visibly slow, cache the wrapped layout until one of these changes:

- document text
- input buffer
- content width
- font id or style

## Error Handling

Wrapping must always make progress. Any invalid or oversized input should still produce a line and advance the byte offset.

Cursor indexes should continue to be clamped by existing cursor helpers. Layout should not emit invalid offset ranges.

## Testing Strategy

Prefer host tests for `WriterWrappedLayout` because wrapping rules are deterministic once the measuring behavior is controlled.

If direct use of `GfxRenderer` makes host tests awkward, introduce a test-only or production measuring seam so tests can provide predictable widths. The seam should stay small: it only needs to answer "how wide is this candidate string?"

Manual simulator checks should include:

- paragraphs with many narrow letters
- paragraphs with many wide letters
- punctuation-heavy prose
- long unbroken words
- UTF-8 words such as `cliche` with an accent
- caret movement across newly wrapped line boundaries
- footer still rendering at the bottom without overlap

## Open Follow-Ups

- insert-at-caret editing
- keyboard `Home`, `End`, `Page Up`, and `Page Down`
- sleep timer reset from real keyboard events
- full-document loading/windowing beyond the 64KB tail
- eventual i18n for Writer-specific footer strings
