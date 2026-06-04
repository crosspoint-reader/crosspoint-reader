# Text Selection API

`ClipSelectionActivity` provides interactive word-level text selection over rendered EPUB pages.
It is designed to be reusable: the same activity drives both the built-in **Save Clipping** feature
and any future text-selection use case (dictionary lookup, custom annotations, etc.).

---

## Core type: `WordRef`

Defined in `src/activities/reader/WordRef.h`.

```cpp
struct WordRef {
  int x, y, w, h;        // bounding box in logical screen coordinates
  int pageIdx;            // 0-based index within the loaded page window
  std::string text;       // raw word text (may start with U+2003 EM SPACE for indented paragraphs)
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;  // true when this word begins a new paragraph
};
```

The caller builds a `std::vector<WordRef>` from the rendered page layout and passes it to the
activity. See `EpubReaderActivity.cpp` (around line 605) for a complete example.

---

## Modes

Set via `ClipSelectionActivity::Config::Mode`.

### `CLIPPING` (default)

Two-step confirm workflow:
1. First `Confirm` press → sets the **start mark** at the cursor word.
2. Second `Confirm` press → confirms the range `[startMark, cursor]`.

Returns `ClippingResult` via `ActivityResult`. Includes full anchor metadata
(`startText`, `endText`, `beforeStartText`, `afterEndText`, `midText`) built by
`ClipTextBuilder::build()`. Use this for `My Clippings.txt` export and annotation matching.

### `WORD_SELECT`

Same two-step workflow, but returns `WordSelectResult` — plain text assembly only,
no anchor building. Useful when the caller only needs the selected words:

```cpp
// dictionary lookup example
ClipSelectionActivity::Config cfg;
cfg.mode = ClipSelectionActivity::Config::Mode::WORD_SELECT;

startActivityForResult(
    std::make_unique<ClipSelectionActivity>(
        renderer, mappedInput, std::move(words),
        /*bookTitle=*/"", /*author=*/"", /*chapter=*/"", /*page=*/0,
        fontId, section, startPage, mTop, mLeft, cfg),
    [](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& sel = std::get<WordSelectResult>(result.data);
        // sel.text       — space-joined selected words
        // sel.fromWordIdx / sel.toWordIdx — indices into the original WordRef vector
    });
```

For **single-word** selection: navigate to the word, press `Confirm` twice (sets mark, then
immediately confirms the one-word range).

---

## Visual style: `WordStyle` + `RenderConfig`

Visual rendering is fully configurable. Each visual state (cursor, selection) has its own
`WordStyle` with combinable bitmask flags:

```cpp
struct WordStyle {
  enum Flags : uint8_t {
    NONE      = 0,
    FILL      = 1 << 0,  // fillRectDither background (color = fillColor)
    INVERT    = 1 << 1,  // solid black background + white text; overrides FILL
    UNDERLINE = 1 << 2,  // horizontal line at font baseline (ascender + 2 px)
    BORDER    = 1 << 3,  // drawRect outline around the word bounding box
  };
  uint8_t flags   = FILL;
  Color fillColor = Color::LightGray;  // only used when FILL flag is set
};
```

Flags combine freely. When both `FILL` and `INVERT` are set, `INVERT` takes priority.

```cpp
struct RenderConfig {
  WordStyle cursor;     // word currently under the cursor
  WordStyle selection;  // words in the confirmed [start, cursor] range
  bool showButtonHints                  = true;
  HalDisplay::RefreshMode refreshMode   = HalDisplay::FAST_REFRESH;
};
```

### Preset examples

```cpp
using WS = ClipSelectionActivity::WordStyle;

// Default clipping (gray fill for both)
cfg.render.cursor    = { WS::FILL, Color::LightGray };
cfg.render.selection = { WS::FILL, Color::LightGray };

// Dictionary: inverted cursor, underlined selection
cfg.render.cursor    = { WS::INVERT };
cfg.render.selection = { WS::FILL | WS::UNDERLINE, Color::LightGray };

// Annotation-style: bordered cursor, underline only for selection
cfg.render.cursor    = { WS::BORDER };
cfg.render.selection = { WS::UNDERLINE };

// High contrast
cfg.render.cursor    = { WS::INVERT | WS::BORDER };
cfg.render.selection = { WS::FILL | WS::UNDERLINE, Color::DarkGray };
```

---

## `ClipTextBuilder`

Declared in `src/clippings/ClipTextBuilder.h`.

```cpp
namespace ClipTextBuilder {
    ClippingResult build(const std::vector<WordRef>& words,
                         int from, int to, int total,
                         int startPageInSection);
}
```

Called internally by `ClipSelectionActivity` in `CLIPPING` mode, but available independently.
Handles:
- **Em-space stripping** — U+2003 prefix used for CSS text-indent paragraphs
- **Hyphen merging** — trailing `-` at line break joined to next word
- **Paragraph newlines** — inserted on em-space, `paragraphStart` flag, or Y-gap between lines
- **Anchor extraction** — `startText` (first 4 words), `endText` (last 4), `midText` (4 around centre),
  `beforeStartText` / `afterEndText` (3 words of context on each side)

---

## Full `Config` reference

```cpp
struct Config {
    enum class Mode { CLIPPING, WORD_SELECT };
    Mode         mode   = Mode::CLIPPING;
    RenderConfig render;          // visual style (cursor + selection WordStyle)
};
```

Default `Config{}` reproduces the original clipping behaviour exactly.

---

## Constructor

```cpp
ClipSelectionActivity(
    GfxRenderer&                     renderer,
    MappedInputManager&              mappedInput,
    std::vector<WordRef>             words,
    std::string                      bookTitle,
    std::string                      author,
    std::string                      chapterTitle,
    int                              pageNumber,        // 1-based, for display only
    int                              fontId,
    Section&                         section,
    int                              startPageInSection,
    int                              marginTop,
    int                              marginLeft,
    Config                           config = {}        // optional, defaults to CLIPPING
);
```
