# Vertical Text (Tategaki) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable natural vertical Japanese text reading in EPUBs with `writing-mode: vertical-rl`, including upright CJK characters, sideways Latin, tate-chu-yoko, punctuation offsets, vertical ruby, and RTL page progression.

**Architecture:** Five layers built bottom-up: (1) CSS detection & settings provide the writing mode signal, (2) the layout engine converts words into vertical columns, (3) the renderer draws glyphs with per-character rotation/offset rules, (4) cache serialization persists vertical layout, (5) page navigation flips for RTL. Each layer is independently testable via build + device verification.

**Tech Stack:** C++20, ESP-IDF/Arduino-ESP32, PlatformIO, expat XML parser, SdFat, custom EpdFont system.

**Testing:** This is ESP32 firmware with no unit test framework. Each task ends with `pio run` (build verification). Functional testing is done on-device with a test EPUB containing `writing-mode: vertical-rl`.

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `lib/Epub/Epub/css/CssStyle.h` | Add `CssWritingMode` enum and field |
| Modify | `lib/Epub/Epub/css/CssParser.cpp` | Parse `writing-mode` CSS property |
| Modify | `lib/Epub/Epub/parsers/ContentOpfParser.h` | Store page-progression-direction |
| Modify | `lib/Epub/Epub/parsers/ContentOpfParser.cpp` | Parse spine `page-progression-direction` attribute |
| Modify | `lib/Epub/Epub/Epub.h` | Expose page-progression-direction |
| Modify | `lib/Epub/Epub/BookMetadataCache.h` | Add pageProgressionRtl to BookMetadata |
| Modify | `src/CrossPointSettings.h` | Add `writingMode` setting |
| Modify | `src/activities/settings/SettingsActivity.cpp` | Add writing mode to Reader settings tab |
| Modify | `lib/I18n/translations/english.yaml` | Add STR_WRITING_MODE, STR_WM_AUTO, etc. |
| Modify | `lib/I18n/translations/japanese.yaml` | Add Japanese translations |
| Create | `lib/GfxRenderer/VerticalTextUtils.h` | Character classification (upright/sideways/tcy), punctuation offset table |
| Modify | `lib/Epub/Epub/ParsedText.h` | Add `VerticalBehavior` enum, `wordVerticalBehaviors` vector, `layoutVerticalColumns()` |
| Modify | `lib/Epub/Epub/ParsedText.cpp` | Implement vertical column layout |
| Modify | `lib/Epub/Epub/blocks/TextBlock.h` | Add `wordYpos` vector, `isVertical` flag |
| Modify | `lib/Epub/Epub/blocks/TextBlock.cpp` | Serialize/deserialize vertical layout data |
| Modify | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` | Add `verticalMode` flag |
| Modify | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | Vertical word grouping in `characterData()`, vertical layout flush |
| Modify | `lib/GfxRenderer/GfxRenderer.h` | Declare `drawTextVertical()` |
| Modify | `lib/GfxRenderer/GfxRenderer.cpp` | Implement `drawTextVertical()` with rotation/offset logic |
| Modify | `lib/Epub/Epub/Section.h` | Add `writingMode` parameter to load/create signatures |
| Modify | `lib/Epub/Epub/Section.cpp` | Add `writingMode` to header, bump `SECTION_FILE_VERSION` to 26 |
| Modify | `src/activities/reader/EpubReaderActivity.cpp` | Resolve effective writing mode, pass to Section, RTL page navigation |
| Modify | `src/activities/reader/EpubReaderActivity.h` | Store resolved writing mode |

---

## Task 1: CssWritingMode Enum & CssStyle Field

**Files:**
- Modify: `lib/Epub/Epub/css/CssStyle.h`

- [ ] **Step 1: Add CssWritingMode enum and field to CssStyle**

In `lib/Epub/Epub/css/CssStyle.h`, add the enum before the `CssStyle` struct:

```cpp
enum class CssWritingMode : uint8_t { HorizontalTb, VerticalRl };
```

Add field inside `CssStyle` struct, after the `display` field:

```cpp
  CssWritingMode writingMode = CssWritingMode::HorizontalTb;
```

Add corresponding flag in `CssPropertyFlags`:

```cpp
  uint8_t writingMode : 1;
```

- [ ] **Step 2: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add lib/Epub/Epub/css/CssStyle.h
git commit -m "✨ CssWritingMode enumとCssStyleフィールドを追加"
```

---

## Task 2: CSS Parser - writing-mode Property

**Files:**
- Modify: `lib/Epub/Epub/css/CssParser.cpp`

- [ ] **Step 1: Add interpretWritingMode helper function**

Add near the other `interpret*` functions (around line 170):

```cpp
static CssWritingMode interpretWritingMode(std::string_view value) {
  const std::string_view v = stripTrailingImportant(value);
  if (v == "vertical-rl") return CssWritingMode::VerticalRl;
  return CssWritingMode::HorizontalTb;
}
```

- [ ] **Step 2: Add writing-mode parsing to parseDeclarationIntoStyle**

Add before the closing `}` of the property chain (around line 347), matching the existing pattern:

```cpp
  } else if (propNameBuf == "writing-mode" || propNameBuf == "-epub-writing-mode" ||
             propNameBuf == "-webkit-writing-mode") {
    style.writingMode = interpretWritingMode(propValueBuf);
    style.defined.writingMode = 1;
  }
```

- [ ] **Step 3: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add lib/Epub/Epub/css/CssParser.cpp
git commit -m "✨ CSSパーサーにwriting-modeプロパティの解析を追加"
```

---

## Task 3: OPF page-progression-direction Detection

**Files:**
- Modify: `lib/Epub/Epub/parsers/ContentOpfParser.h`
- Modify: `lib/Epub/Epub/parsers/ContentOpfParser.cpp`
- Modify: `lib/Epub/Epub/BookMetadataCache.h`
- Modify: `lib/Epub/Epub/Epub.h`

- [ ] **Step 1: Add pageProgressionRtl to BookMetadata**

In `lib/Epub/Epub/BookMetadataCache.h`, add to the `BookMetadata` struct:

```cpp
  bool pageProgressionRtl = false;
```

- [ ] **Step 2: Add field to ContentOpfParser**

In `lib/Epub/Epub/parsers/ContentOpfParser.h`, add member variable:

```cpp
  bool pageProgressionRtl = false;
```

- [ ] **Step 3: Parse page-progression-direction in spine startElement**

In `lib/Epub/Epub/parsers/ContentOpfParser.cpp`, in the spine `startElement` handler (around line 129), add attribute parsing after `self->state = IN_SPINE;`:

```cpp
    // Parse page-progression-direction
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "page-progression-direction") == 0 && strcmp(atts[i + 1], "rtl") == 0) {
        self->pageProgressionRtl = true;
      }
    }
```

- [ ] **Step 4: Propagate to BookMetadata after parsing**

Find where `ContentOpfParser` results are transferred to `BookMetadata` (in `Epub.cpp` `parseContentOpf()`). Add:

```cpp
  bookMetadata.pageProgressionRtl = visitor.pageProgressionRtl;
```

- [ ] **Step 5: Expose via Epub class**

In `lib/Epub/Epub/Epub.h`, add public method:

```cpp
  bool isPageProgressionRtl() const;
```

Implement in `Epub.cpp` using `bookMetadataCache`.

- [ ] **Step 6: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 7: Commit**

```bash
git add lib/Epub/Epub/parsers/ContentOpfParser.h lib/Epub/Epub/parsers/ContentOpfParser.cpp \
  lib/Epub/Epub/BookMetadataCache.h lib/Epub/Epub/Epub.h lib/Epub/Epub.cpp
git commit -m "✨ OPFのpage-progression-direction=rtl検出を追加"
```

---

## Task 4: User Setting - Writing Mode

**Files:**
- Modify: `src/CrossPointSettings.h`
- Modify: `src/activities/settings/SettingsActivity.cpp`
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/japanese.yaml`

- [ ] **Step 1: Add enum and setting field to CrossPointSettings.h**

```cpp
  enum WRITING_MODE : uint8_t { WM_AUTO = 0, WM_HORIZONTAL = 1, WM_VERTICAL = 2, WRITING_MODE_COUNT };
  uint8_t writingMode = WM_AUTO;
```

- [ ] **Step 2: Add i18n keys**

In `lib/I18n/translations/english.yaml`:
```yaml
STR_WRITING_MODE: "Writing Mode"
STR_WM_AUTO: "Auto"
STR_WM_HORIZONTAL: "Horizontal"
STR_WM_VERTICAL: "Vertical"
```

In `lib/I18n/translations/japanese.yaml`:
```yaml
STR_WRITING_MODE: "組方向"
STR_WM_AUTO: "自動"
STR_WM_HORIZONTAL: "横書き"
STR_WM_VERTICAL: "縦書き"
```

- [ ] **Step 3: Run i18n generator**

```bash
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

- [ ] **Step 4: Add setting to Reader tab in SettingsActivity**

In `src/activities/settings/SettingsActivity.cpp`, add to the Reader settings list (find the `readerSettings` vector or equivalent). Follow the existing pattern for enum settings:

```cpp
  {StrId::STR_WRITING_MODE, SettingType::ENUM,
   &CrossPointSettings::writingMode,
   {StrId::STR_WM_AUTO, StrId::STR_WM_HORIZONTAL, StrId::STR_WM_VERTICAL}},
```

- [ ] **Step 5: Add writingMode to settings serialization**

Find the `saveToFile()` / `loadFromFile()` methods in CrossPointSettings and add `writingMode` to the save/load list, following the existing pattern.

- [ ] **Step 6: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 7: Commit**

```bash
git add src/CrossPointSettings.h src/activities/settings/SettingsActivity.cpp \
  lib/I18n/translations/english.yaml lib/I18n/translations/japanese.yaml
git commit -m "✨ ユーザー設定に組方向（横書き/縦書き）切り替えを追加"
```

---

## Task 5: VerticalTextUtils - Character Classification & Punctuation Table

**Files:**
- Create: `lib/GfxRenderer/VerticalTextUtils.h`

- [ ] **Step 1: Create VerticalTextUtils.h**

```cpp
#pragma once

#include <cstdint>

namespace VerticalTextUtils {

// Character behavior in vertical text layout
enum class VerticalBehavior : uint8_t {
  Upright,      // CJK ideographs, kana - draw normally, advance downward
  Sideways,     // Latin letters, 3+ digit numbers - rotate 90 CW
  TateChuYoko,  // 1-2 digit numbers - horizontal-in-vertical
};

// Punctuation offset for vertical text (ratio of character size, in 1/8 units)
struct PunctuationOffset {
  uint32_t codepoint;
  int8_t dxEighths;  // horizontal offset in 1/8 of charWidth
  int8_t dyEighths;  // vertical offset in 1/8 of charHeight
  bool rotate;       // true = rotate 90 CW (e.g. long vowel mark)
};

// Punctuation that needs repositioning in vertical text.
// Offsets are in 1/8 of character dimension to avoid floating point.
static constexpr PunctuationOffset VERTICAL_PUNCTUATION[] = {
    {0x3001, 3, -3, false},   // 、 ideographic comma → upper-right
    {0x3002, 3, -3, false},   // 。 ideographic period → upper-right
    {0xFF0C, 3, -3, false},   // ， fullwidth comma → upper-right
    {0xFF0E, 3, -3, false},   // ． fullwidth period → upper-right
    {0x30FC, 0, 0, true},     // ー katakana long vowel mark → rotate
    {0x2014, 0, 0, true},     // — em dash → rotate
    {0x2015, 0, 0, true},     // ― horizontal bar → rotate
    {0x2026, 0, 0, true},     // … ellipsis → rotate
    {0xFF5E, 0, 0, true},     // ～ fullwidth tilde → rotate
};
static constexpr int VERTICAL_PUNCTUATION_COUNT =
    sizeof(VERTICAL_PUNCTUATION) / sizeof(VERTICAL_PUNCTUATION[0]);

// Look up punctuation offset. Returns nullptr if no special handling needed.
inline const PunctuationOffset* getVerticalPunctuationOffset(uint32_t cp) {
  for (int i = 0; i < VERTICAL_PUNCTUATION_COUNT; i++) {
    if (VERTICAL_PUNCTUATION[i].codepoint == cp) return &VERTICAL_PUNCTUATION[i];
  }
  return nullptr;
}

// Determine how a codepoint should behave in vertical text.
// This is called per-character during layout (not rendering).
inline bool isUprightInVertical(uint32_t cp) {
  // CJK Unified Ideographs
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Extension A
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // Hiragana
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // CJK Symbols and Punctuation
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Fullwidth Forms
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  // CJK Compatibility Ideographs
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Halfwidth/Fullwidth Forms (CJK part)
  if (cp >= 0xFFE0 && cp <= 0xFFEF) return true;
  // Enclosed CJK Letters
  if (cp >= 0x3200 && cp <= 0x32FF) return true;
  // CJK Compatibility
  if (cp >= 0x3300 && cp <= 0x33FF) return true;
  // Bopomofo
  if (cp >= 0x3100 && cp <= 0x312F) return true;
  // Hangul
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;

  return false;
}

}  // namespace VerticalTextUtils
```

- [ ] **Step 2: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS (header-only, no link errors)

- [ ] **Step 3: Commit**

```bash
git add lib/GfxRenderer/VerticalTextUtils.h
git commit -m "✨ 縦書き用の文字分類・句読点オフセットテーブルを追加"
```

---

## Task 6: TextBlock - Vertical Layout Data

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`

- [ ] **Step 1: Add vertical fields to TextBlock.h**

Add member variables after `wordXpos`:

```cpp
  std::vector<int16_t> wordYpos;    // vertical layout: y position within column
  bool isVertical = false;          // true when this block was laid out vertically
```

Update constructor to accept optional vertical data:

```cpp
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<int16_t> word_ypos = {}, bool vertical = false)
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle),
        wordYpos(std::move(word_ypos)),
        isVertical(vertical) {}
```

Add accessors:

```cpp
  const std::vector<int16_t>& getWordYpos() const { return wordYpos; }
  bool getIsVertical() const { return isVertical; }
```

- [ ] **Step 2: Update serialize() in TextBlock.cpp**

After writing `wordStyles`, add:

```cpp
  // Vertical layout data
  serialization::writePod(file, isVertical);
  if (isVertical) {
    for (auto y : wordYpos) serialization::writePod(file, y);
  }
```

- [ ] **Step 3: Update deserialize() in TextBlock.cpp**

After reading `wordStyles`, add:

```cpp
  // Vertical layout data
  bool vertical = false;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  if (vertical) {
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
  }
```

Update the constructor call at the end:

```cpp
  return std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle,
                    std::move(wordYpos), vertical));
```

- [ ] **Step 4: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/blocks/TextBlock.h lib/Epub/Epub/blocks/TextBlock.cpp
git commit -m "✨ TextBlockに縦書きレイアウトデータ（wordYpos）を追加"
```

---

## Task 7: Section Cache - Writing Mode Parameter

**Files:**
- Modify: `lib/Epub/Epub/Section.h`
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: Bump SECTION_FILE_VERSION and update header**

In `Section.cpp`, change:

```cpp
constexpr uint8_t SECTION_FILE_VERSION = 26;
```

Update `HEADER_SIZE` to include `sizeof(bool)` for writingMode:

```cpp
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t);
//                                                                                ^^^^^^^^^^^^^ writingMode added
```

- [ ] **Step 2: Add writingMode parameter to all Section method signatures**

In `Section.h`, add `bool verticalMode` parameter to `loadSectionFile`, `createSectionFile`, and `writeSectionFileHeader`.

In `Section.cpp`, update `writeSectionFileHeader` to write the new field:

```cpp
  serialization::writePod(file, verticalMode);
```

Update `loadSectionFile` to read and compare:

```cpp
  bool fileVerticalMode;
  serialization::readPod(file, fileVerticalMode);
  // Add to parameter mismatch check:
  if (... || verticalMode != fileVerticalMode) {
```

Update `createSectionFile` to pass verticalMode through to the header writer and the parser.

- [ ] **Step 3: Update static_assert for HEADER_SIZE**

Update the static_assert to include the new `sizeof(bool)` for verticalMode.

- [ ] **Step 4: Update all callers in EpubReaderActivity.cpp**

Find all calls to `section->loadSectionFile(...)` and `section->createSectionFile(...)`. Add the `verticalMode` parameter (for now, pass `false` — Task 11 will wire in the real value).

- [ ] **Step 5: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/Section.h lib/Epub/Epub/Section.cpp \
  src/activities/reader/EpubReaderActivity.cpp
git commit -m "🎨 Sectionキャッシュにwriting modeパラメータを追加（v26）"
```

---

## Task 8: ParsedText - Vertical Column Layout

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h`
- Modify: `lib/Epub/Epub/ParsedText.cpp`

- [ ] **Step 1: Add VerticalBehavior tracking to ParsedText.h**

Add include:

```cpp
#include "../../lib/GfxRenderer/VerticalTextUtils.h"
```

Add member variable:

```cpp
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
```

Add overloaded addWord:

```cpp
  void addWord(std::string word, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vBehavior,
               bool underline = false, bool attachToPrevious = false);
```

Add vertical layout method:

```cpp
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight, uint16_t columnWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processColumn);
```

- [ ] **Step 2: Implement addWord overload in ParsedText.cpp**

```cpp
void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle,
                         const VerticalTextUtils::VerticalBehavior vBehavior,
                         const bool underline, const bool attachToPrevious) {
  addWord(std::move(word), fontStyle, underline, attachToPrevious);
  if (wordVerticalBehaviors.capacity() == 0) {
    wordVerticalBehaviors.reserve(800);
  }
  wordVerticalBehaviors.push_back(vBehavior);
}
```

- [ ] **Step 3: Implement layoutVerticalColumns()**

Core algorithm:
1. Calculate word heights (CJK: advanceX as height, Sideways: getTextWidth as height, TateChuYoko: advanceY as height)
2. Break into columns when cumulative height exceeds `columnHeight`
3. For each column, compute wordYpos (top-to-bottom) and produce a TextBlock with `isVertical=true`

```cpp
void ParsedText::layoutVerticalColumns(const GfxRenderer& renderer, const int fontId,
                                       const uint16_t columnHeight, const uint16_t columnWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processColumn) {
  if (words.empty()) return;

  // Ensure SD card font metrics are loaded
  if (renderer.isSdCardFont(fontId)) {
    std::string allText;
    for (const auto& w : words) { allText += w; allText += ' '; }
    renderer.ensureSdCardFontReady(fontId, allText.c_str());
  }

  const int lineHeight = renderer.getLineHeight(fontId);

  // Calculate word heights for vertical layout
  std::vector<uint16_t> wordHeights;
  wordHeights.reserve(words.size());
  for (size_t i = 0; i < words.size(); i++) {
    auto vb = (i < wordVerticalBehaviors.size()) ? wordVerticalBehaviors[i]
                                                  : VerticalTextUtils::VerticalBehavior::Upright;
    switch (vb) {
      case VerticalTextUtils::VerticalBehavior::Sideways:
        // Rotated text: height = horizontal text width
        wordHeights.push_back(renderer.getTextWidth(fontId, words[i].c_str(), wordStyles[i]));
        break;
      case VerticalTextUtils::VerticalBehavior::TateChuYoko:
        // Horizontal-in-vertical: height = line height (one character slot)
        wordHeights.push_back(lineHeight);
        break;
      default:
        // Upright CJK: height = advance (use advanceX, CJK is square)
        wordHeights.push_back(renderer.getTextWidth(fontId, words[i].c_str(), wordStyles[i]));
        break;
    }
  }

  // Break into columns
  size_t columnStart = 0;
  int currentY = 0;

  for (size_t i = 0; i < words.size(); i++) {
    if (currentY + wordHeights[i] > columnHeight && i > columnStart) {
      // Emit column from columnStart to i-1
      std::vector<std::string> colWords(words.begin() + columnStart, words.begin() + i);
      std::vector<int16_t> colYpos;
      std::vector<int16_t> colXpos;  // all zero for vertical (column x set by page layout)
      std::vector<EpdFontFamily::Style> colStyles(wordStyles.begin() + columnStart, wordStyles.begin() + i);
      colYpos.reserve(colWords.size());
      colXpos.resize(colWords.size(), 0);

      int y = 0;
      for (size_t j = columnStart; j < i; j++) {
        colYpos.push_back(static_cast<int16_t>(y));
        y += wordHeights[j];
      }

      processColumn(std::make_shared<TextBlock>(
          std::move(colWords), std::move(colXpos), std::move(colStyles),
          blockStyle, std::move(colYpos), true));

      columnStart = i;
      currentY = 0;
    }
    currentY += wordHeights[i];
  }

  // Emit remaining words as final column
  if (columnStart < words.size()) {
    std::vector<std::string> colWords(words.begin() + columnStart, words.end());
    std::vector<int16_t> colYpos;
    std::vector<int16_t> colXpos;
    std::vector<EpdFontFamily::Style> colStyles(wordStyles.begin() + columnStart, wordStyles.end());
    colYpos.reserve(colWords.size());
    colXpos.resize(colWords.size(), 0);

    int y = 0;
    for (size_t j = columnStart; j < words.size(); j++) {
      colYpos.push_back(static_cast<int16_t>(y));
      y += wordHeights[j];
    }

    processColumn(std::make_shared<TextBlock>(
        std::move(colWords), std::move(colXpos), std::move(colStyles),
        blockStyle, std::move(colYpos), true));
  }

  // Consume words (same pattern as layoutAndExtractLines)
  words.clear();
  wordStyles.clear();
  wordContinues.clear();
  wordVerticalBehaviors.clear();
}
```

- [ ] **Step 4: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp
git commit -m "✨ ParsedTextに縦書きカラムレイアウトエンジンを追加"
```

---

## Task 9: ChapterHtmlSlimParser - Vertical Word Grouping

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`

- [ ] **Step 1: Add verticalMode flag to parser**

In `ChapterHtmlSlimParser.h`, add member:

```cpp
  bool verticalMode = false;
```

Update the constructor to accept this parameter.

- [ ] **Step 2: Modify characterData() for vertical word grouping**

In `characterData()` (around line 900), modify the CJK character handling to pass VerticalBehavior when in vertical mode:

```cpp
    if (isCjkCodepointForSplit(cp)) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      char cjkWord[5] = {0};
      for (int j = 0; j < charLen && j < 4; j++) {
        cjkWord[j] = s[i + j];
      }
      if (self->verticalMode) {
        self->currentTextBlock->addWord(cjkWord, EpdFontFamily::REGULAR,
                                        VerticalTextUtils::VerticalBehavior::Upright);
      } else {
        self->currentTextBlock->addWord(cjkWord, EpdFontFamily::REGULAR);
      }
      i += charLen;
      continue;
    }
```

- [ ] **Step 3: Add Latin/digit grouping for vertical mode**

In `flushPartWordBuffer()`, when `verticalMode` is true, classify the buffered word:
- Count consecutive ASCII digits. If the entire word is 1-2 digits → TateChuYoko
- If the word contains Latin letters or 3+ digits → Sideways
- Otherwise → Upright

```cpp
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (partWordBufferIndex == 0) return;
  partWordBuffer[partWordBufferIndex] = '\0';

  if (verticalMode) {
    // Classify the word for vertical behavior
    bool allDigits = true;
    int charCount = 0;
    for (int i = 0; i < partWordBufferIndex; i++) {
      if ((partWordBuffer[i] & 0xC0) != 0x80) charCount++;  // count UTF-8 start bytes
      if (partWordBuffer[i] < '0' || partWordBuffer[i] > '9') allDigits = false;
    }
    auto vb = VerticalTextUtils::VerticalBehavior::Sideways;  // default for Latin
    if (allDigits && charCount <= 2) {
      vb = VerticalTextUtils::VerticalBehavior::TateChuYoko;
    }
    currentTextBlock->addWord(partWordBuffer, effectiveFontStyle(), vb, effectiveUnderline(), nextWordContinues);
  } else {
    currentTextBlock->addWord(partWordBuffer, effectiveFontStyle(), effectiveUnderline(), nextWordContinues);
  }

  partWordBufferIndex = 0;
  nextWordContinues = true;
}
```

- [ ] **Step 4: Use layoutVerticalColumns in the 750-word flush**

In `characterData()`, around line 932, add vertical branch:

```cpp
  if (normalFlush || earlyFlush) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    if (self->verticalMode) {
      const uint16_t effectiveHeight = self->viewportHeight;
      const uint16_t columnWidth = renderer.getLineHeight(self->fontId);
      self->currentTextBlock->layoutVerticalColumns(
          self->renderer, self->fontId, effectiveHeight, columnWidth,
          [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); });
    } else {
      const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                          ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                          : self->viewportWidth;
      self->currentTextBlock->layoutAndExtractLines(
          self->renderer, self->fontId, effectiveWidth,
          [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
    }
  }
```

- [ ] **Step 5: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h \
  lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
git commit -m "✨ HTMLパーサーに縦書きモードのワードグループ化を追加"
```

---

## Task 10: GfxRenderer - drawTextVertical()

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.h`
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`

- [ ] **Step 1: Declare drawTextVertical in GfxRenderer.h**

```cpp
  void drawTextVertical(int fontId, int x, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
```

- [ ] **Step 2: Implement drawTextVertical in GfxRenderer.cpp**

Core logic: iterate codepoints, for each character:
- Check punctuation table for offset/rotation
- CJK (upright): draw normally at (x, yPos), advance yPos downward
- Punctuation with rotate flag: use drawTextRotated90CW pixel logic
- Punctuation with offset: apply (dx, dy) based on character dimensions

```cpp
void GfxRenderer::drawTextVertical(const int fontId, const int x, const int y, const char* text,
                                   const bool black, const EpdFontFamily::Style style) const {
  if (!text || *text == '\0') return;

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const int lineHeight = getLineHeight(fontId);
  int yPos = y;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);

  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    // Look up glyph
    const EpdGlyph* glyph = nullptr;
    const uint8_t* bitmap = nullptr;
    bool is2Bit = false;
    // ... (resolve glyph from fontMap/sdCardFonts/externalFont, same pattern as drawText)

    if (!glyph) continue;

    const int charAdvance = fp4::toPixel(glyph->advanceX);
    const auto* punctOffset = VerticalTextUtils::getVerticalPunctuationOffset(cp);

    if (punctOffset && punctOffset->rotate) {
      // Rotated punctuation: use 90 CW rotation logic
      // Draw at (x, yPos) with rotation transform: (glyphX, glyphY) -> (glyphY, -glyphX)
      // (Same pixel loop as drawTextRotated90CW)
      renderCharRotated90CW(glyph, bitmap, is2Bit, x, yPos, black);
      yPos += charAdvance;
    } else if (punctOffset) {
      // Offset punctuation: draw with position adjustment
      int dx = punctOffset->dxEighths * charAdvance / 8;
      int dy = punctOffset->dyEighths * charAdvance / 8;
      renderCharUpright(glyph, bitmap, is2Bit, x + dx, yPos + dy, black);
      yPos += charAdvance;
    } else {
      // Normal upright character
      renderCharUpright(glyph, bitmap, is2Bit, x, yPos, black);
      yPos += charAdvance;
    }
  }
}
```

Note: `renderCharUpright` and `renderCharRotated90CW` are helper functions extracted from the existing `drawText` and `drawTextRotated90CW` pixel loops. This refactoring keeps the main functions clean.

- [ ] **Step 3: Update TextBlock::render() for vertical mode**

In `TextBlock.cpp`, modify `render()` to check `isVertical` and use `drawTextVertical()`:

```cpp
void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const int viewportWidth) const {
  for (size_t i = 0; i < words.size(); i++) {
    if (isVertical && i < wordYpos.size()) {
      renderer.drawTextVertical(fontId, x + wordXpos[i], y + wordYpos[i],
                                words[i].c_str(), true, wordStyles[i]);
    } else {
      renderer.drawText(fontId, x + wordXpos[i], y, words[i].c_str(), true, wordStyles[i]);
    }
  }
}
```

- [ ] **Step 4: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 5: Commit**

```bash
git add lib/GfxRenderer/GfxRenderer.h lib/GfxRenderer/GfxRenderer.cpp \
  lib/Epub/Epub/blocks/TextBlock.cpp
git commit -m "✨ GfxRendererに縦書きテキスト描画（drawTextVertical）を追加"
```

---

## Task 11: EpubReaderActivity - Writing Mode Resolution & Page Navigation

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Add writing mode resolution to EpubReaderActivity.h**

Add member:

```cpp
  bool verticalMode = false;  // resolved effective writing mode for current book
```

- [ ] **Step 2: Resolve effective writing mode in render()**

In `EpubReaderActivity::render()`, after the epub is loaded and before section building, resolve the effective writing mode:

```cpp
  // Resolve effective writing mode
  if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
    verticalMode = true;
  } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
    verticalMode = false;
  } else {
    // Auto: check CSS (via section's embedded style) and OPF hints
    verticalMode = epub->isPageProgressionRtl() &&
                   (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" ||
                    epub->getLanguage() == "zh" || epub->getLanguage() == "zho");
  }
```

Note: Full CSS-level detection (per-section writing-mode) requires passing the resolved CssStyle through the parser. For the initial implementation, OPF-level + language detection covers the common case (entire book is vertical). Per-section CSS detection can be added in a follow-up.

- [ ] **Step 3: Pass verticalMode to Section methods**

Update the calls to `section->loadSectionFile(...)` and `section->createSectionFile(...)` to pass `verticalMode` instead of the `false` placeholder from Task 7.

- [ ] **Step 4: RTL page navigation**

In the page turn logic (around line 196+), wrap direction when `verticalMode`:

```cpp
  if (prevTriggered) {
    pageTurn(verticalMode ? true : false);   // vertical: prev button = forward
  } else {
    pageTurn(verticalMode ? false : true);   // vertical: next button = backward
  }
```

- [ ] **Step 5: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 6: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "✨ EpubReaderActivityに縦書きモード解決とRTLページ送りを追加"
```

---

## Task 12: Vertical Page Layout - Column Positioning

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`

- [ ] **Step 1: Modify addLineToPage for vertical column placement**

In vertical mode, `addLineToPage()` needs to position columns from right to left instead of lines top to bottom.

Find `addLineToPage()` and add a vertical branch. The column's x-position should start at `viewportWidth - columnWidth` and decrement for each new column. When x goes below 0, start a new page.

```cpp
void ChapterHtmlSlimParser::addLineToPage(const std::shared_ptr<TextBlock>& textBlock) {
  if (verticalMode) {
    const int columnWidth = renderer.getLineHeight(fontId);
    const int columnSpacing = columnWidth / 4;  // gap between columns

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;
    }

    // Check if column fits
    if (currentPageNextX < 0) {
      // Page full — emit and start new page
      completePageFn(std::move(currentPage));
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;
    }

    // Set column x-position in the TextBlock's wordXpos
    // (wordXpos stores the column's x offset, wordYpos stores per-character y within column)
    // We need to update wordXpos to reflect the column's x position on the page
    // This is handled via PageLine's xPos parameter
    auto pageLine = std::make_unique<PageLine>(textBlock, currentPageNextX, 0);
    currentPage->addElement(std::move(pageLine));
    currentPageNextX -= (columnWidth + columnSpacing);
  } else {
    // Existing horizontal logic (unchanged)
    // ...
  }
}
```

- [ ] **Step 2: Also handle makePages() for vertical**

Find `makePages()` (called at end of parsing for remaining text) and add the same vertical branch for `layoutVerticalColumns`.

- [ ] **Step 3: Build verification**

```bash
pio run 2>&1 | tail -5
```

Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
git commit -m "✨ 縦書き時のカラム配置（右→左）をページレイアウトに統合"
```

---

## Task 13: End-to-End Integration & Device Testing

**Files:** No new code changes — this is a verification task.

- [ ] **Step 1: Full build**

```bash
pio run -t clean && pio run 2>&1 | tail -10
```

Expected: SUCCESS with no warnings in modified files.

- [ ] **Step 2: Flash to device**

```bash
pio run -t upload --upload-port /dev/tty.usbmodem101
```

- [ ] **Step 3: Test with vertical EPUB**

Use `omoide.epub` (青空文庫, has `writing-mode: vertical-rl` in CSS):
1. Clear reading cache (Settings → Clear Cache)
2. Open the book
3. Verify text displays vertically (CJK upright, columns right-to-left)
4. Turn pages — verify RTL progression
5. Check punctuation positioning (。、)
6. Switch setting to "横書き" — verify horizontal display
7. Switch back to "自動" — verify vertical resumes

- [ ] **Step 4: Test with horizontal EPUB**

Open any English EPUB:
1. Verify no regression — horizontal text renders normally
2. Writing mode setting "自動" should keep it horizontal
3. Force "縦書き" — verify it applies vertical layout

- [ ] **Step 5: Commit any fixes discovered during testing**

```bash
git add -A
git commit -m "🐛 縦書き表示の統合テストで発見した問題を修正"
```

---

## Done Criteria

- [ ] `writing-mode: vertical-rl` CSSが自動検出され、縦書きで表示される
- [ ] CJK文字が正立、ラテン文字が90°回転で表示される
- [ ] 2桁以下の数字が縦中横で表示される
- [ ] 句読点（。、）が右上にオフセットされる
- [ ] ページ送りがRTL（右→左）で動作する
- [ ] 設定から手動で横書き/縦書きを切り替えられる
- [ ] キャッシュが writing mode 変更時に自動再構築される
- [ ] 横書きEPUBに回帰バグがない
- [ ] ビルドが警告なしで成功する

※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
