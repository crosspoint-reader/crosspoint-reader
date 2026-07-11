# Korean title fallback

This patch keeps the built-in UI fonts as the primary fonts, and uses the
currently selected SD-card reader `.cpfont` only for user-content titles and
file names that contain codepoints missing from the UI font.

## Traced rendering paths

- Home recent title: `HomeActivity::render()` passes the latest book title to
  `BaseTheme::drawHeader()` when `homeContinueReadingInMenu` is active, and to
  `drawRecentBookCover()` for cover tiles.
- Recent books list: `RecentBooksActivity::render()` passes `RecentBook::title`
  and `RecentBook::author` to `drawList()`.
- File browser list: `FileBrowserActivity::render()` passes `getFileName()` to
  `drawList()`. The extension value remains normal UI text.
- Width and truncation: theme code now uses
  `GfxRenderer::getContentTextWidth()`, `truncatedContentText()`, and
  `wrappedContentText()` only when a row/header/cover string is marked as user
  content.

## Fallback rule

For each codepoint:

1. Use the built-in UI font when it contains a real glyph.
2. Otherwise use the selected SD-card reader font when it is loaded and contains
   the glyph.
3. Otherwise keep the existing replacement-glyph behavior.

The renderer uses the same rule for measurement and drawing. `EpdFont::hasGlyph`
and `SdCardFont::hasGlyph` intentionally test real coverage and do not treat
U+FFFD replacement fallback as a match.

## Manual test matrix

- English title: `The Hobbit` should use UI glyphs only.
- Korean title: `한글 제목` should use SD `.cpfont` glyphs for Hangul.
- Mixed title: `English 한글 123` should use UI glyphs for Latin/digits and SD
  glyphs for Hangul.
- No SD font selected: affected screens must still open and show the existing
  replacement-glyph behavior for missing UI glyphs.
- Missing or corrupt `.cpfont`: `SdCardFontSystem::ensureLoaded()` must fail
  closed and leave the UI rendering path usable.

No flashing, unlock/re-lock, partition change, or OTA install is part of this
test procedure.
