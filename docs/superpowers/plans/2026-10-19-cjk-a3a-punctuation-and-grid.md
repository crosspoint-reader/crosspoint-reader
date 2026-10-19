# CJK typography A3a: per-language profile, punctuation widths, flush justification, grid — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Chinese and Japanese books get CJK punctuation set at the widths their language's layout requirements give it, justified CJK lines that end flush, and characters on a whole-em grid; every book gets its typographic rules from one per-language profile.

**Architecture:** A `TypographyProfile`, resolved once per section build from the book's `dc:language`, travels from `Section.cpp` through `ChapterHtmlSlimParser` to every `ParsedText`. `CjkPunctuation` classifies CJK marks and gives each language's widths (rules written from W3C JLREQ/CLREQ). `ParsedText::addWord` makes every adjustable mark a token of its own (glued where kinsoku holds it), `calculateWordWidths` narrows marks to their mid-line widths and records how a line edge changes them, all four line breakers and `extractLine` measure lines with those edge widths, `extractLine` places a narrowed mark by its ink, spreads the justification remainder on CJK lines, and the grid rounds a zh/ja block's width and indent to whole ems. A3b (push-in and the CJK–Latin gap) follows in its own plan.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), host tests with GoogleTest/CMake (`test/`), Python fixture generators (`test/epubs/`).

**Spec:** `docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md` (this branch). It was built from a prototype (`proto/cjk-a3`, worktree `C:/tmp/wt-cjk-a3-proto`); every code block below is cut from it, and the prototype passes every test this plan names.

## Global Constraints

- Latin books lay out exactly as before: the non-CJK goldens in `test/epub_pipeline/goldens/` must not change (spec section 6). Every task that touches layout runs the golden test and checks that only `test_cjk_*` goldens changed.
- The CJK goldens change only where a rule of spec section 5 explains it; regenerate them with `UPDATE_GOLDENS=1` and look at the diff before committing.
- No new settings (spec section 6).
- Heap: no new per-token field beyond one byte; the extra punctuation tokens are allowed; tables are flash only (spec section 6). Layout scratch vectors are members reused across paragraphs, as `wordWidths_` is.
- The punctuation rules are written from JLREQ/CLREQ; KOReader's crengine tables are a cross-check only (GPL; the firmware is MIT) (spec 5.2). Do not copy crengine code.
- Section file version: 95 (from 94), bumped once, in Task 3.
- Comments follow the repository style: say what the code does and why, no history narration.
- clang-format 21 is what CI runs (`bin/clang-format-fix`); version 22 differs on comment alignment. Run the version-21 check before pushing.
- Host tests build and run from Git Bash (`cmake --build build/test -j 8 -- -k 0`, `ctest`); on Windows four tests fail on master too (`BookFontsTest` ×2, `LongChapterPageTables` ×2) and are not this plan's.
- Firmware: `default`, `x4pro` and `lilygo_t5s3` must build (`PLATFORMIO_CORE_DIR=C:/pio`, one `pio run` at a time).
- Code containing backslash escapes (`"\xE4\xB8\x80"` for 一 appears in `ParsedText.cpp` and the tests) goes in with the Write or Edit tool. A bash heredoc or a non-raw Python string rewrites the escapes, and the result compiles: the replay of this plan lost a debugging round to a corrupted 一 ("not whole ems of 26").

## Review Focus

1. **A heading or other sized block in a Chinese or Japanese book**: the grid rounds to the block's own em (its font-size multiplier), not the body's. Pinned in Task 6: `CjkGrid.AHeadingsGridIsInItsOwnEms`.
2. **A block beside a float** (an image floated next to text): it keeps its float-narrowed widths and gets no grid. Pinned in Task 6: `CjkGrid.ABlockBesideAFloatKeepsItsOwnWidth`.
3. **A page-bottom retry** (the parser asks for a hyphenated line again without hyphenation): words are merged back and measured again, and the line-edge widths must stay in step with the tokens. Pinned in Task 4: `CjkPunctuationWidths.APageBottomRetryKeepsTheMarksWidths`.
4. **Chinese dialogue with curly quotes (”“) in a CJK font**: a narrowed neighbour must not stop counting as CJK punctuation. Pinned in Task 4: `CjkPunctuationWidths.ACurlyQuotePairInCjkTextSetsAQuarterEmEachSide`.
5. **A CJK book read in a font without CJK glyphs, or CJK marks drawn narrow by a Latin font**: nothing is narrowed (a mark must measure at least 4/5 of the em of 一 in its own face and size), and no grid is applied when 一 measures 0. The host shims cannot produce a narrow CJK mark, so the reviewer checks `applyPunctuationWidths`' full-width test and the grid's `em > 0` guard by reading; Task 7 checks it on the device with a built-in Latin reader font.

---

### Task 1: The typography profile, from the book's language to every ParsedText

**Files:**
- Create: `lib/Epub/Epub/TypographyProfile.h`, `lib/Epub/Epub/TypographyProfile.cpp`
- Create: `test/typography/CMakeLists.txt`, `test/typography/TypographyTest.cpp` (the profile tests now; Task 2 adds the punctuation tests)
- Modify: `lib/Epub/Epub/hyphenation/Hyphenator.h`, `lib/Epub/Epub/hyphenation/Hyphenator.cpp`
- Modify: `lib/Epub/Epub/ParsedText.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`
- Modify: `test/CMakeLists.txt` (`add_subdirectory(typography)` after `cjk_layout`); `test/cjk_layout/CMakeLists.txt`, `test/word_size_layout/CMakeLists.txt`, `test/epub_pipeline/CMakeLists.txt` (add `${REPO_ROOT}/lib/Epub/Epub/TypographyProfile.cpp` after `ParsedText.cpp`)

**Interfaces:**
- Produces: `struct TypographyProfile` with enums `Breaking`, `Justify`, `Punctuation { SimplifiedChinese, TraditionalChinese, Japanese }`, `Direction`; fields `breaking`, `justify`, `punctuation`, `cjkLatinGap`, `wholeEmGrid`, `direction`, `verticalAllowed`, `hyphenator`; `static TypographyProfile forLanguage(const std::string& tag)`.
- Produces: `static const LanguageHyphenator* Hyphenator::forLanguage(const std::string& lang)`, `static void Hyphenator::setLanguageHyphenator(const LanguageHyphenator*)`.
- Produces: `void ParsedText::setTypography(const TypographyProfile&)` (survives `reset()`), member `TypographyProfile typography_`; `void ChapterHtmlSlimParser::setTypography(const TypographyProfile&)`, member `typography_`.

- [ ] **Step 1: Write the failing profile tests.** Create `test/typography/TypographyTest.cpp` with the `TypographyProfile.*` tests of the file below (the `CjkPunctuation.*` tests and the `CjkPunctuation.h` include come in Task 2), and `test/typography/CMakeLists.txt` as below without the `CjkPunctuation.cpp` line; register the directory in `test/CMakeLists.txt`.

The full test file, as it stands after Task 2:
```cpp
// The typography profile a book's language picks (spec section 3), and the punctuation widths of each language's
// table (spec section 5.2, from JLREQ and CLREQ).
#include <gtest/gtest.h>

#include "Epub/CjkPunctuation.h"
#include "Epub/TypographyProfile.h"

namespace {

using Table = TypographyProfile::Punctuation;
using CjkPunctuation::Class;

TEST(TypographyProfile, ChineseAndJapanesePickTheirTablesTheGridAndTheGap) {
  const TypographyProfile ja = TypographyProfile::forLanguage("ja");
  EXPECT_EQ(ja.punctuation, Table::Japanese);
  EXPECT_EQ(ja.breaking, TypographyProfile::Breaking::BetweenCharacters);
  EXPECT_EQ(ja.justify, TypographyProfile::Justify::BetweenCharacters);
  EXPECT_TRUE(ja.wholeEmGrid);
  EXPECT_TRUE(ja.cjkLatinGap);
  EXPECT_TRUE(ja.verticalAllowed);

  EXPECT_EQ(TypographyProfile::forLanguage("zh").punctuation, Table::SimplifiedChinese);
  EXPECT_EQ(TypographyProfile::forLanguage("zh-CN").punctuation, Table::SimplifiedChinese);
  EXPECT_TRUE(TypographyProfile::forLanguage("zh-CN").wholeEmGrid);
}

TEST(TypographyProfile, TraditionalChineseByScriptRegionOrClassicalChinese) {
  for (const char* tag : {"zh-Hant", "zh-TW", "zh_HK", "ZH-mo", "zh-Hant-CN", "lzh"}) {
    EXPECT_EQ(TypographyProfile::forLanguage(tag).punctuation, Table::TraditionalChinese) << tag;
  }
  // An explicit script subtag wins over the region.
  EXPECT_EQ(TypographyProfile::forLanguage("zh-Hans-TW").punctuation, Table::SimplifiedChinese);
}

TEST(TypographyProfile, KoreanKeepsWholeWordsWithoutGridOrGap) {
  const TypographyProfile ko = TypographyProfile::forLanguage("ko-KR");
  EXPECT_EQ(ko.breaking, TypographyProfile::Breaking::KoreanWords);
  EXPECT_EQ(ko.justify, TypographyProfile::Justify::WordSpaces);
  EXPECT_EQ(ko.punctuation, Table::SimplifiedChinese);
  EXPECT_FALSE(ko.wholeEmGrid);
  EXPECT_FALSE(ko.cjkLatinGap);
  EXPECT_FALSE(ko.verticalAllowed);
}

TEST(TypographyProfile, OtherLanguagesKeepLatinRulesAndTheirHyphenation) {
  const TypographyProfile en = TypographyProfile::forLanguage("en-US");
  EXPECT_EQ(en.breaking, TypographyProfile::Breaking::AtSpaces);
  EXPECT_EQ(en.punctuation, Table::SimplifiedChinese) << "CJK punctuation in any book";
  EXPECT_FALSE(en.wholeEmGrid);
  EXPECT_FALSE(en.cjkLatinGap);
  EXPECT_NE(en.hyphenator, nullptr);
  EXPECT_EQ(TypographyProfile::forLanguage("").hyphenator, nullptr);
  EXPECT_FALSE(TypographyProfile::forLanguage("zhx").wholeEmGrid) << "only the primary subtag zh is Chinese";
}

TEST(TypographyProfile, EveryLanguageRunsLeftToRightThisRound) {
  for (const char* tag : {"en", "ja", "zh-Hant", "ko", "ar", "he"}) {
    EXPECT_EQ(TypographyProfile::forLanguage(tag).direction, TypographyProfile::Direction::LeftToRight) << tag;
  }
}

TEST(CjkPunctuation, ClassesFollowJlreq) {
  EXPECT_EQ(CjkPunctuation::classOf(0x300C), Class::Opening);           // 「
  EXPECT_EQ(CjkPunctuation::classOf(0x300D), Class::Closing);           // 」
  EXPECT_EQ(CjkPunctuation::classOf(0xFF01), Class::Dividing);          // ！
  EXPECT_EQ(CjkPunctuation::classOf(0x30FB), Class::MiddleDot);         // ・
  EXPECT_EQ(CjkPunctuation::classOf(0x3002), Class::FullStop);          // 。
  EXPECT_EQ(CjkPunctuation::classOf(0x3001), Class::Comma);             // 、
  EXPECT_EQ(CjkPunctuation::classOf(0x3000), Class::IdeographicSpace);  // U+3000
  EXPECT_EQ(CjkPunctuation::classOf(0xFF02), Class::AmbiguousQuote);    // ＂
  EXPECT_EQ(CjkPunctuation::classOf(0x201C), Class::Opening);           // “
  EXPECT_EQ(CjkPunctuation::classOf(0x6F22), Class::None);              // 漢
  EXPECT_EQ(CjkPunctuation::classOf('A'), Class::None);
  EXPECT_TRUE(CjkPunctuation::isCurlyQuote(0x2019));
  EXPECT_FALSE(CjkPunctuation::isCurlyQuote(0x300D));
}

// Widths in eighths of the mark's advance: {forced, minimum}.
void expectWidth(const Table table, const Class mark, const Class neighbour, const bool edge, const int forced,
                 const int minimum) {
  const CjkPunctuation::Width w = CjkPunctuation::widthOf(table, mark, neighbour, edge);
  EXPECT_EQ(w.forced, forced);
  EXPECT_EQ(w.minimum, minimum);
}

TEST(CjkPunctuation, SimplifiedChineseHalvesMarksAtLineEdgesAndInRuns) {
  const Table sc = Table::SimplifiedChinese;
  expectWidth(sc, Class::Closing, Class::None, true, 4, 4);       // 」 ending a line
  expectWidth(sc, Class::Opening, Class::None, true, 4, 4);       // 「 starting a line
  expectWidth(sc, Class::Closing, Class::FullStop, false, 4, 4);  // 」 in 」。: all but the last of a run
  expectWidth(sc, Class::FullStop, Class::None, false, 8, 4);     // 。 before text: full, may halve
  expectWidth(sc, Class::Opening, Class::Opening, false, 4, 4);   // the second 「 of 「「
  expectWidth(sc, Class::Opening, Class::None, false, 8, 4);      // 「 after text
  expectWidth(sc, Class::Closing, Class::Opening, false, 6, 6);   // 」 in 」「: a quarter em each side
  expectWidth(sc, Class::Opening, Class::Closing, false, 6, 6);   // 「 in 」「
  expectWidth(sc, Class::IdeographicSpace, Class::None, true, 8, 8);
}

TEST(CjkPunctuation, TraditionalChineseKeepsCentredMarksFullWidth) {
  const Table tc = Table::TraditionalChinese;
  expectWidth(tc, Class::FullStop, Class::None, true, 8, 6);      // 。 at a line end stays full
  expectWidth(tc, Class::Closing, Class::None, true, 8, 4);       // a bracket may halve on demand
  expectWidth(tc, Class::Closing, Class::FullStop, false, 8, 4);  // no forced run rule
  expectWidth(tc, Class::Comma, Class::None, false, 8, 6);
}

TEST(CjkPunctuation, JapaneseKeepsTheFullStopAndLetsLineEndsChoose) {
  const Table ja = Table::Japanese;
  expectWidth(ja, Class::FullStop, Class::None, false, 8, 6);     // 。 keeps its blank mid-line
  expectWidth(ja, Class::FullStop, Class::Closing, false, 4, 4);  // 。」: solid before the bracket
  expectWidth(ja, Class::FullStop, Class::None, true, 8, 4);      // half or full at a line end
  expectWidth(ja, Class::Comma, Class::None, true, 8, 4);
  expectWidth(ja, Class::Closing, Class::None, true, 8, 4);
  expectWidth(ja, Class::Opening, Class::None, true, 4, 4);       // a line start sets solid
  expectWidth(ja, Class::Opening, Class::FullStop, false, 4, 4);  // 。「: solid after 。
  expectWidth(ja, Class::Closing, Class::Opening, false, 6, 6);   // 」「
  expectWidth(ja, Class::Dividing, Class::None, false, 8, 6);     // ！？ keep their width
  expectWidth(ja, Class::MiddleDot, Class::None, false, 8, 4);    // ・：； may halve
}

}  // namespace
```

```cmake
add_executable(TypographyTest
  TypographyTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/TypographyProfile.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TypographyTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TypographyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TypographyTest)
```

- [ ] **Step 2: Run it to see it fail.** `cmake -S test -B build/test && cmake --build build/test --target TypographyTest` — Expected: compile error, `TypographyProfile.h` not found.

- [ ] **Step 3: Implement the profile.** `TypographyProfile.h` and `.cpp` exactly as below. In `Hyphenator.h` add, after `setPreferredLanguage`:
```cpp
  // The patterns for a language tag ("en", "en-US", "eng"), null when the language has none. The book's
  // TypographyProfile holds this, and the section build installs it with setLanguageHyphenator().
  static const LanguageHyphenator* forLanguage(const std::string& lang);
  static void setLanguageHyphenator(const LanguageHyphenator* hyphenator) { cachedHyphenator_ = hyphenator; }
```
and in `Hyphenator.cpp`, after `setPreferredLanguage`: `const LanguageHyphenator* Hyphenator::forLanguage(const std::string& lang) { return hyphenatorForLanguage(lang); }` (`setPreferredLanguage` stays; tests use it).

```cpp
#pragma once

#include <cstdint>
#include <string>

class LanguageHyphenator;

// How a language's text is laid out, chosen once per section build from the book's language tag (OPF
// dc:language). Layout asks the profile instead of testing for "a CJK book" in several places, so a language
// added or corrected is one entry in TypographyProfile.cpp.
//
// Rules that follow from the characters alone stay with the characters: CJK line breaking and kinsoku
// (CjkLineBreak.h) and the Simplified Chinese punctuation widths on CJK punctuation apply in every book. The
// profile decides what needs the language.
struct TypographyProfile {
  // Where lines may break. Informational in this round: CjkLineBreak applies its rules by character.
  enum class Breaking : uint8_t { AtSpaces, BetweenCharacters, KoreanWords };
  // What a justified line stretches. Informational in this round, as Breaking.
  enum class Justify : uint8_t { WordSpaces, BetweenCharacters };
  // The punctuation width table (CjkPunctuation.h).
  enum class Punctuation : uint8_t { SimplifiedChinese, TraditionalChinese, Japanese };
  // Which way characters run inside a line. Only LeftToRight is laid out yet; RightToLeft (Arabic, Hebrew)
  // will land in the base direction CompiledContent.h reserves.
  enum class Direction : uint8_t { LeftToRight, RightToLeft };

  Breaking breaking = Breaking::AtSpaces;
  Justify justify = Justify::WordSpaces;
  Punctuation punctuation = Punctuation::SimplifiedChinese;
  // A quarter em between Han or kana and a Latin letter or digit with no space between them (PR A3b).
  bool cjkLatinGap = false;
  // Text width and first-line indent rounded down to whole ems, so CJK characters sit in columns.
  bool wholeEmGrid = false;
  Direction direction = Direction::LeftToRight;
  // A book may set this language vertically (CSS writing-mode). Nothing reads it yet.
  bool verticalAllowed = false;
  // The language's hyphenation patterns, null when it has none.
  const LanguageHyphenator* hyphenator = nullptr;

  // The profile for a BCP 47 tag ("ja", "zh-Hant-TW", "zh_CN", "en-US"); Latin rules for an empty or unknown
  // one. Case and '-'/'_' do not matter.
  static TypographyProfile forLanguage(const std::string& tag);
};
```

```cpp
#include "TypographyProfile.h"

#include "hyphenation/Hyphenator.h"

namespace {

// The tag lowercased, with '_' read as '-' (zh_TW is common in OPF files).
std::string normalised(const std::string& tag) {
  std::string out;
  out.reserve(tag.size());
  for (char c : tag) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_') c = '-';
    out.push_back(c);
  }
  return out;
}

bool hasPrimary(const std::string& tag, const char* primary) {
  const std::string p = primary;
  return tag.compare(0, p.size(), p) == 0 && (tag.size() == p.size() || tag[p.size()] == '-');
}

bool hasSubtag(const std::string& tag, const char* subtag) {
  const std::string needle = std::string("-") + subtag;
  for (size_t at = tag.find(needle); at != std::string::npos; at = tag.find(needle, at + 1)) {
    const size_t end = at + needle.size();
    if (end == tag.size() || tag[end] == '-') return true;
  }
  return false;
}

// Chinese written in traditional characters: an explicit script subtag decides, else the region (Taiwan, Hong
// Kong, Macao). Classical Chinese (lzh) is traditional as well. As KOReader's TextLangCfg.
bool isTraditionalChinese(const std::string& tag) {
  if (hasPrimary(tag, "lzh")) return true;
  if (hasSubtag(tag, "hant")) return true;
  if (hasSubtag(tag, "hans")) return false;
  return hasSubtag(tag, "tw") || hasSubtag(tag, "hk") || hasSubtag(tag, "mo");
}

}  // namespace

TypographyProfile TypographyProfile::forLanguage(const std::string& tag) {
  const std::string t = normalised(tag);
  TypographyProfile p;
  p.hyphenator = Hyphenator::forLanguage(tag);

  const bool chinese = hasPrimary(t, "zh") || hasPrimary(t, "lzh");
  if (chinese || hasPrimary(t, "ja")) {
    p.breaking = Breaking::BetweenCharacters;
    p.justify = Justify::BetweenCharacters;
    p.punctuation = hasPrimary(t, "ja")       ? Punctuation::Japanese
                    : isTraditionalChinese(t) ? Punctuation::TraditionalChinese
                                              : Punctuation::SimplifiedChinese;
    p.cjkLatinGap = true;
    p.wholeEmGrid = true;
    p.verticalAllowed = true;
  } else if (hasPrimary(t, "ko")) {
    // Korean spaces its words and uses Western punctuation (KLREQ): the Simplified Chinese table only meets
    // the rare CJK mark, and there is no grid and no CJK-Latin gap.
    p.breaking = Breaking::KoreanWords;
  }
  return p;
}
```

- [ ] **Step 4: Wire it through.**
  - `ParsedText.h`: `#include "TypographyProfile.h"`; member `TypographyProfile typography_;` with the comment "The book's typography (TypographyProfile.h): Latin rules until the parser sets the book's."; public `void setTypography(const TypographyProfile& typography) { typography_ = typography; }` next to `setBlockStyle`, commented "Survives reset(): it is the book's, not the block's."
  - `ChapterHtmlSlimParser.h`: `void setTypography(const TypographyProfile& typography) { typography_ = typography; }` after `setFontFamilies` ("The book's typography (TypographyProfile.h), handed to every block the parser lays out; Latin rules by default."), member `TypographyProfile typography_;` after `fontSizeLadder_`.
  - `ChapterHtmlSlimParser.cpp`, the three places a `ParsedText` is made: after the `currentTextBlock` reset-or-new in `startNewTextBlock` add `if (currentTextBlock) currentTextBlock->setTypography(typography_);`; after `row.cells.back().text = std::unique_ptr<ParsedText>(new ParsedText(false, false));` add `row.cells.back().text->setTypography(self->typography_);`; after `open.text = std::unique_ptr<ParsedText>(new ParsedText(false, false));` add `open.text->setTypography(typography_);`.
  - `Section.cpp`: `#include "TypographyProfile.h"`; replace `Hyphenator::setPreferredLanguage(epub->getLanguage());` with
```cpp
  // The book's language decides its typography (TypographyProfile.h): hyphenation patterns, CJK punctuation widths,
  // the CJK grid.
  const TypographyProfile typography = TypographyProfile::forLanguage(epub->getLanguage());
  Hyphenator::setLanguageHyphenator(typography.hyphenator);
  st.visitor->setTypography(typography);
```

- [ ] **Step 5: Run the tests.** `cmake --build build/test --target TypographyTest EpubPipelineTest CjkLayoutTest WordSizeLayoutTest` then `build/test/typography/TypographyTest.exe` — Expected: the 5 profile tests PASS. `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='-BookFontsTest.*:LongChapterPageTables.*'` — Expected: all PASS, no golden changes (`git status test/epub_pipeline/goldens` clean): nothing reads the profile yet but the hyphenator.

- [ ] **Step 6: Commit.** `git commit -m "feat(epub): a typography profile per book language"`

---

### Task 2: CJK punctuation classes and each language's widths

**Files:**
- Create: `lib/Epub/Epub/CjkPunctuation.h`, `lib/Epub/Epub/CjkPunctuation.cpp`
- Modify: `test/typography/TypographyTest.cpp` (the `CjkPunctuation.*` tests), `test/typography/CMakeLists.txt` (`CjkPunctuation.cpp`)
- Modify: `test/cjk_layout/CMakeLists.txt`, `test/word_size_layout/CMakeLists.txt`, `test/epub_pipeline/CMakeLists.txt` (add `${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp` after `CjkLineBreak.cpp`)

**Interfaces:**
- Consumes: `TypographyProfile::Punctuation` (Task 1).
- Produces: `enum class CjkPunctuation::Class : uint8_t { None, Opening, Closing, Dividing, MiddleDot, FullStop, Comma, IdeographicSpace, AmbiguousQuote }`; `Class classOf(uint32_t cp)`; `bool isCurlyQuote(uint32_t cp)`; `struct Width { uint8_t forced; uint8_t minimum; }` (eighths of the mark's advance); `Width widthOf(TypographyProfile::Punctuation table, Class mark, Class neighbour, bool atLineEdge)` — an opening mark's neighbour is the character before it, every other mark's the one after it; `atLineEdge` means a line starts (opening) or ends (others) at the mark.

- [ ] **Step 1: Add the failing tests:** `CjkPunctuation.ClassesFollowJlreq`, `SimplifiedChineseHalvesMarksAtLineEdgesAndInRuns`, `TraditionalChineseKeepsCentredMarksFullWidth`, `JapaneseKeepsTheFullStopAndLetsLineEndsChoose` and `expectWidth`, as in the Task 1 file listing.
- [ ] **Step 2: Run them to see them fail.** Expected: compile error, `CjkPunctuation.h` not found.
- [ ] **Step 3: Implement** both files exactly as below. The values are the spec's (5.2); every rule carries its JLREQ/CLREQ reading in the comment above it.

```cpp
#pragma once

#include <cstdint>

#include "TypographyProfile.h"

// The widths of full-width CJK punctuation (spec section 5.2). A full-width mark is ink on one half of its em
// and blank on the other; set next to other punctuation or at a line edge, part of that blank goes, as the
// layout requirements of each language describe (W3C JLREQ, CLREQ). The rules are written from those documents;
// KOReader's crengine tables (textlang.cpp) were the cross-check, not the source.
namespace CjkPunctuation {

// The JLREQ classes of the marks a table adjusts.
enum class Class : uint8_t {
  None,              // text: Han, kana, Latin, and marks no table adjusts
  Opening,           // 「『（【〔〈《〘〖｟〝［｛, and the curly “‘ where full-width
  Closing,           // 」』）】〕〉》〙〗｠〞〟］｝, and the curly ”’ where full-width
  Dividing,          // ！？‼⁇⁈⁉
  MiddleDot,         // ・：；
  FullStop,          // 。．
  Comma,             // 、，
  IdeographicSpace,  // U+3000, which keeps its full width
  AmbiguousQuote,    // ＂＇, which may open or close
};

Class classOf(uint32_t cp);

// The curly quotes are CJK punctuation only where the font draws them full-width (a CJK font in CJK text); in a
// Latin font they are narrow and are left alone. The caller checks the width.
bool isCurlyQuote(uint32_t cp);

// A width in eighths of the mark's full advance (8: unchanged). `forced` always applies; `minimum` is how far
// the line may shrink the mark when it needs room (push-in, PR A3b).
struct Width {
  uint8_t forced;
  uint8_t minimum;
};

// The width of `mark` beside its neighbour. An opening mark looks at the character BEFORE it, every other mark
// at the character AFTER it, as JLREQ's spacing does. `neighbour` is None for text or a space there.
// `atLineEdge`: a line starts at the opening mark, or ends after the other mark, so the neighbour is the edge.
Width widthOf(TypographyProfile::Punctuation table, Class mark, Class neighbour, bool atLineEdge);

}  // namespace CjkPunctuation
```

```cpp
#include "CjkPunctuation.h"

namespace CjkPunctuation {

using Table = TypographyProfile::Punctuation;

Class classOf(const uint32_t cp) {
  switch (cp) {
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0xFF08:  // （
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x3018:  // 〘
    case 0x3016:  // 〖
    case 0xFF5F:  // ｟
    case 0x301D:  // 〝
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x201C:  // “
    case 0x2018:  // ‘
      return Class::Opening;
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0xFF09:  // ）
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x3019:  // 〙
    case 0x3017:  // 〗
    case 0xFF60:  // ｠
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x201D:  // ”
    case 0x2019:  // ’
      return Class::Closing;
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0x203C:  // ‼
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
      return Class::Dividing;
    case 0x30FB:  // ・
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
      return Class::MiddleDot;
    case 0x3002:  // 。
    case 0xFF0E:  // ．
      return Class::FullStop;
    case 0x3001:  // 、
    case 0xFF0C:  // ，
      return Class::Comma;
    case 0x3000:
      return Class::IdeographicSpace;
    case 0xFF02:  // ＂
    case 0xFF07:  // ＇
      return Class::AmbiguousQuote;
    default:
      return Class::None;
  }
}

bool isCurlyQuote(const uint32_t cp) { return cp == 0x201C || cp == 0x201D || cp == 0x2018 || cp == 0x2019; }

namespace {

bool closesOrSeparates(const Class c) {
  return c == Class::Closing || c == Class::Dividing || c == Class::MiddleDot || c == Class::FullStop ||
         c == Class::Comma;
}

// CLREQ, mainland books: punctuation is half-width at a line's start (opening) or end (the rest), and in a run of
// marks only one keeps its blank -- the first of a run of opening marks, the last of the rest. Between a closing
// and an opening mark half an em of blank stays, a quarter on each side. Elsewhere a mark may halve on demand.
// Also the table for the CJK punctuation of a book in any other language.
Width simplifiedChinese(const Class mark, const Class neighbour, const bool atLineEdge) {
  if (mark == Class::Opening) {
    if (atLineEdge) return {4, 4};
    if (neighbour == Class::Opening || neighbour == Class::AmbiguousQuote) return {4, 4};
    if (closesOrSeparates(neighbour)) return {6, 6};
    return {8, 4};
  }
  if (closesOrSeparates(mark)) {
    if (atLineEdge) return {4, 4};
    if (neighbour == Class::Opening) return {6, 6};
    if (closesOrSeparates(neighbour) || neighbour == Class::AmbiguousQuote) return {4, 4};
    return {8, 4};
  }
  return {8, 8};
}

// CLREQ, Taiwan and Hong Kong: the marks are centred in their em, so they keep their full width, at a line's edges
// too. They give way only on demand: a bracket (ink on one side) to half next to punctuation or a line edge,
// everything else to three quarters.
Width traditionalChinese(const Class mark, const Class neighbour, const bool atLineEdge) {
  if (mark == Class::Opening) {
    if (atLineEdge) return {8, 4};
    return neighbour == Class::None || neighbour == Class::Closing ? Width{8, 6} : Width{8, 4};
  }
  if (mark == Class::Closing) {
    if (atLineEdge) return {8, 4};
    return neighbour == Class::None || neighbour == Class::Opening ? Width{8, 6} : Width{8, 4};
  }
  if (closesOrSeparates(mark) || mark == Class::AmbiguousQuote) return {8, 6};
  return {8, 8};
}

// JLREQ. An opening bracket at a line start, and a mark after 。、 or another opening bracket, sets solid (half).
// Between a closing and an opening bracket half an em of blank stays; elsewhere a quarter em of a bracket's blank
// may go on demand. 。 keeps its blank mid-line; 、 may halve. At a line end 。、 and a closing bracket are half or
// full (push-in decides, PR A3b). ！？ keep their full width but for a quarter on demand; ・：； may halve.
// Before a closing bracket or another of 。、・：；, a mark sets solid.
Width japanese(const Class mark, const Class neighbour, const bool atLineEdge) {
  switch (mark) {
    case Class::Opening:
      if (atLineEdge) return {4, 4};
      switch (neighbour) {
        case Class::Opening:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        case Class::Closing:
          return {6, 6};
        default:
          return {8, 6};
      }
    case Class::Closing:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Opening:
          return {6, 6};
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        default:
          return {8, 6};
      }
    case Class::Dividing:
      return {8, 6};
    case Class::MiddleDot:
      return {8, 4};
    case Class::FullStop:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
          return {4, 4};
        case Class::IdeographicSpace:
          return {8, 4};
        default:
          return {8, 6};
      }
    case Class::Comma:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        case Class::Opening:
          return {8, 6};
        default:
          return {8, 4};
      }
    default:
      return {8, 8};
  }
}

}  // namespace

Width widthOf(const Table table, const Class mark, const Class neighbour, const bool atLineEdge) {
  switch (table) {
    case Table::TraditionalChinese:
      return traditionalChinese(mark, neighbour, atLineEdge);
    case Table::Japanese:
      return japanese(mark, neighbour, atLineEdge);
    case Table::SimplifiedChinese:
    default:
      return simplifiedChinese(mark, neighbour, atLineEdge);
  }
}

}  // namespace CjkPunctuation
```

- [ ] **Step 4: Run.** `build/test/typography/TypographyTest.exe` — Expected: 9 tests PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(epub): CJK punctuation classes and each language's widths (JLREQ, CLREQ)"`

---

### Task 3: Every adjustable CJK mark is a token of its own

A token is drawn with one `drawText` at its font's advances, so only a mark that is a token can later be drawn narrower (spec 5.2). Kinsoku still holds: the mark is glued (`continues = true`) wherever A2 allowed no break.

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`addWord`; `#include "CjkPunctuation.h"`)
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 95)
- Create: `test/cjk_layout/CjkTypographyTest.cpp` (registered in `test/cjk_layout/CMakeLists.txt` after `CjkLayoutTest.cpp`)
- Modify: `test/cjk_layout/CjkLayoutTest.cpp` (two expectations), CJK goldens

**Interfaces:**
- Consumes: `CjkPunctuation::classOf` (Task 2), `CjkLineBreak::breakByteOffsets` (A2).
- Produces: token boundaries — a CJK break stays `{continues=false, noSpaceBefore=true}`, the other edges of a mark are `{continues=true, noSpaceBefore=false}`.

- [ ] **Step 1: Write the failing tests.** Create `CjkTypographyTest.cpp` with the header below and the two tests after it. (Task 4 adds the x-position assertion to the first test.)

```cpp
// CJK punctuation widths, line-edge widths, flush justification and the whole-em grid (spec sections 5.1, 5.2,
// 5.5), through the real ParsedText and TextBlock against the fixed-width shim of test/word_size_layout: every
// codepoint 10 px (so 一 is a 10 px em and every CJK mark is full-width), a space 5 px. The shim draws a CJK
// opening bracket's ink on the right half of its advance, a closing bracket, 。 and 、 on the left half.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/TypographyProfile.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
};

BlockStyle aligned(const CssTextAlign alignment, const int16_t indent = 0) {
  BlockStyle bs;
  bs.alignment = alignment;
  bs.textIndent = indent;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
          line.continues.push_back(block->wordContinues(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

using Words = std::vector<std::vector<std::string>>;
using X = std::vector<int16_t>;

}  // namespace
```

```cpp
TEST(CjkPunctuationWidths, EachMarkIsATokenGluedWhereKinsokuHoldsIt) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"字", "」", "。", "次"}));
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, true, true, false}));
}
TEST(CjkPunctuationWidths, ALatinWordKeepsItsCurlyQuotes) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE2\x80\x9CHello\xE2\x80\x9D", EpdFontFamily::REGULAR);  // “Hello”
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words.size(), 1u);
}
```

- [ ] **Step 2: Run to see it fail.** `cmake --build build/test --target CjkLayoutTest && build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='CjkPunctuationWidths.*'` — Expected: `EachMarkIsATokenGluedWhereKinsokuHoldsIt` FAILS (words `字」。` and `次`).

- [ ] **Step 3: Implement the cuts in `addWord`.** Replace everything from the comment that starts "Chinese and Japanese are written without spaces" through `if (!reserveWords(cjkBreaks.size() + 1)) return;` (the comment, the `cjkBreaks` declaration and the reserve) with the block below, which declares `cjkBreaks` again itself:

```cpp
  // Chinese and Japanese are written without spaces: a word with CJK characters becomes one token
  // per break the CJK rules allow, so a line can end between its characters. Latin words get no
  // offsets and stay whole. Our implementation of crosspoint-reader PR #2288 ("feat: Support for
  // Korean line breaks and glyph spacing", Justin Mitchell, 22f35750).
  //
  // Every CJK mark whose width the punctuation table can change is a token of its own as well, glued
  // where kinsoku forbids a break (字。 stays together, but 。 gets its own x position): a token is
  // drawn at its font's advances, so only a mark that is a token can be drawn narrower (spec 5.2).
  const std::vector<size_t> cjkBreaks = CjkLineBreak::breakByteOffsets(word);
  // Only in a word with CJK text: a Latin word keeps its curly quotes inside it.
  std::vector<size_t> markEdges;  // byte offsets around each adjustable mark, glued unless a break is allowed
  bool hasCjk = false;
  for (const auto* q = reinterpret_cast<const unsigned char*>(word.c_str()); *q && !hasCjk;) {
    hasCjk = utf8IsCjkBreakable(utf8NextCodepoint(&q));
  }
  if (hasCjk) {
    const auto* const begin = reinterpret_cast<const unsigned char*>(word.c_str());
    const auto* p = begin;
    while (*p) {
      const size_t at = static_cast<size_t>(p - begin);
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;
      const CjkPunctuation::Class c = CjkPunctuation::classOf(cp);
      if (c == CjkPunctuation::Class::None || c == CjkPunctuation::Class::IdeographicSpace ||
          c == CjkPunctuation::Class::AmbiguousQuote) {
        continue;
      }
      const size_t after = static_cast<size_t>(p - begin);
      if (at > 0) markEdges.push_back(at);
      if (after < word.size()) markEdges.push_back(after);
    }
  }
  // The token boundaries: the CJK breaks, then each mark edge that is not already one (glued).
  struct Cut {
    size_t at;
    bool breakable;
  };
  std::vector<Cut> cuts;
  cuts.reserve(cjkBreaks.size() + markEdges.size());
  for (const size_t at : cjkBreaks) cuts.push_back({at, true});
  for (const size_t at : markEdges) {
    if (std::find(cjkBreaks.begin(), cjkBreaks.end(), at) == cjkBreaks.end()) cuts.push_back({at, false});
  }
  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at < b.at; });
  cuts.erase(std::unique(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at == b.at; }),
             cuts.end());
  if (!reserveWords(cuts.size() + 1)) return;
```

and the push loop at the end of `addWord` with:

```cpp
  if (cuts.empty()) {
    pushToken(std::move(word), combinedStyle, boundary, size, visibleOffset, fontSlot);
    return;
  }
  size_t start = 0;
  uint32_t offset = visibleOffset;
  for (size_t i = 0; i <= cuts.size(); ++i) {
    const size_t end = i < cuts.size() ? cuts[i].at : word.size();
    pushToken(word.substr(start, end - start), combinedStyle, boundary, size, offset, fontSlot);
    // The next token starts where this one's visible bytes end (KOSync positions).
    offset += static_cast<uint32_t>(VisibleText::visibleBytes(word.data() + start, static_cast<int>(end - start)));
    if (i < cuts.size()) {
      // A CJK break is a breakable zero-width gap; a mark's other edges are glued (no space, no break).
      boundary = cuts[i].breakable ? TokenBoundary::Boundary{false, true} : TokenBoundary::Boundary{true, false};
    }
    start = end;
  }
}
```

- [ ] **Step 4: Update A2's two token expectations in `CjkLayoutTest.cpp`:** `CjkKinsoku.ClosingPunctuationNeverStartsALine` now expects `{{"一", "二", "三", "四"}, {"五", "。", "六"}}` (comment: "。 is glued to 五, so the line ends before 五 rather than leave 。 at the start of the next."); `CjkKinsoku.AnOpeningBracketNeverEndsALine` expects `{{"一", "二", "三", "四"}, {"「", "五", "」"}}`.

- [ ] **Step 5: Bump the section version** to 95 in `Section.cpp`, with the comment lines "v95: CJK punctuation is narrowed by the book's / table, a mark is a token of its own, CJK lines / justify flush, zh/ja text sits on a whole-em grid" above the v94 entry (one bump for the whole of A3a).

- [ ] **Step 6: Run.** `CjkLayoutTest.exe` — Expected: all PASS. Pipeline: `UPDATE_GOLDENS=1 build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*MatchesGolden*'`, then `git status test/epub_pipeline/goldens` — Expected: only `test_cjk_*` goldens changed (tokens split at marks, x positions unchanged). `test_cjk_ko_hyeon` is among them: Hangul counts as CJK text, so the curly quotes inside its words split into glued tokens too. Then the full pipeline run without `UPDATE_GOLDENS` — Expected: all PASS.

- [ ] **Step 7: Commit.** `git commit -m "feat(epub): every adjustable CJK mark is a token of its own"`

---

### Task 4: Mark widths, at mid-line and at the line edges, placed by their ink

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/GfxRenderer/GfxRenderer.h`, `lib/GfxRenderer/GfxRenderer.cpp` (`getGlyphInk`)
- Modify: `test/word_size_layout/GfxRenderer.h`, `test/epub_pipeline/GfxRenderer.h` (host shims)
- Modify: `test/cjk_layout/CjkTypographyTest.cpp`, `test/cjk_layout/CjkLayoutTest.cpp` (one width)
- Modify: `test/epub_pipeline/CjkRulesTest.cpp`; create `test/epubs/make_test_cjk_zh_punctuation.py` and its book `test/epubs/test_cjk_zh_punctuation.epub`; CJK goldens

**Interfaces:**
- Consumes: Task 2's `widthOf`, Task 1's `typography_.punctuation`, Task 3's mark tokens.
- Produces: `bool GfxRenderer::getGlyphInk(int fontId, uint32_t cp, EpdFontFamily::Style style, int& inkLeft, int& inkWidth, int& advance) const` (pixels at the size `fontId` draws, its base scale applied as `getTextAdvanceX` does; false when the font has no glyph).
- Produces (ParsedText, private): `std::vector<int16_t> lineStartTrim_, lineEndTrim_` (per token: width at a line start / end minus width mid-line), `std::vector<uint8_t> markClass_` (scratch), `void applyPunctuationWidths(const GfxRenderer&, int fontId, std::vector<uint16_t>& widths)`, `int withEdgeTrims(int naturalWidth, size_t first, size_t last) const` (= `naturalWidth + lineStartTrim_[first] + lineEndTrim_[last]`).

- [ ] **Step 1: Write the failing tests.** In `CjkTypographyTest.cpp` add the x-position assertion to `EachMarkIsATokenGluedWhereKinsokuHoldsIt`:
```cpp
  // 」 before 。 is half-width (all but the last of a run), 。 before text keeps its width.
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 25}));
```
and these tests:
```cpp
TEST(CjkPunctuationWidths, ACurlyQuotePairInCjkTextSetsAQuarterEmEachSide) {
  // ”“ in Chinese dialogue, at 160 % (16 px glyphs; the shim draws curly quotes full-width): ” before an opening
  // mark and “ after a closing one are 6/8 each, 12 px. The “ judges ” by its full width: narrowed to 12 px, ” is
  // below the 4/5 em a curly quote needs to count as CJK punctuation.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE5\xA5\xBD\xE2\x80\x9D\xE2\x80\x9C\xE5\xAD\x97", EpdFontFamily::REGULAR, false, false,
               160);  // 好”“字
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  // “ keeps its ink (the right half): drawn 4 px left of its 12 px box at 28.
  EXPECT_EQ(lines[0].xpos, (X{0, 16, 24, 40}));
}
TEST(CjkPunctuationWidths, AMarkEndingALineIsHalfWidthSoTheLineHoldsIt) {
  // 字字字字。 is 45 px with the line-end 。 at half width: it fits a 45 px line. Measured at full width the line
  // would end before 字。.
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
    EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字", "字", "。"}, {"字", "字"}}))
        << (hyphenation ? "greedy breaker" : "DP breaker");
  }
}
TEST(CjkPunctuationWidths, AnOpeningBracketStartingALineIsHalfWidthAndDrawnByItsInk) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字字字字「字」", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 40);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "字", "字", "字"}, {"「", "字", "」"}}));
  // 「 keeps only its ink (the right half of its em): drawn 5 px left of the line start, so the ink starts there.
  EXPECT_EQ(lines[1].xpos, (X{-5, 5, 15}));
}
TEST(CjkPunctuationWidths, TraditionalChineseKeepsItsCentredMarksFullWidth) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("zh-TW"));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkPunctuationWidths, JapaneseLetsAFullStopEndingALineStayFull) {
  // JLREQ: 。 at a line end is half or full, and it stays full unless push-in needs it (PR A3b), so 字。 does not
  // fit after 字字字.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字"}, {"字", "。", "字", "字"}}));
}
TEST(CjkPunctuationWidths, APageBottomRetryKeepsTheMarksWidths) {
  // The retry re-lays a hyphenated line with its words whole: it merges tokens back and measures again, so the
  // line-edge widths must stay in step with the tokens. The last line's 」 is half-width before 。 as without a retry.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Justify));
  for (const char* word : {"가나", "다라마바사아", "자」。차"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  std::vector<Line> lines;
  bool retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 62,
      [&](std::unique_ptr<TextBlock> block, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {  // what the parser does at a page bottom
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines), (Words{{"가나"}, {"다라마바사아"}, {"자", "」", "。", "차"}}));
  EXPECT_EQ(lines[2].xpos, (X{0, 10, 15, 25}));
}
```
(the last one needs `#include "Epub/hyphenation/Hyphenator.h"`). In `CjkLayoutTest.cpp`, `CjkTokens.EachLineStartsAtItsFirstTokensVisibleOffset` lays out at 20 px instead of 25 (on 25 px 漢字。 now shares a line: the line-end 。 is half-width); its comment says so.

- [ ] **Step 2: Run to see them fail.** `cmake --build build/test --target CjkLayoutTest && build/test/cjk_layout/CjkLayoutTest.exe` — Expected: the new width tests FAIL on their x positions and line splits (every mark is still full width), and `EachLineStartsAtItsFirstTokensVisibleOffset` still passes at 20 px.

- [ ] **Step 3: Implement.**
  - **Shims.** Both host `GfxRenderer.h` files include `"Epub/CjkPunctuation.h"` (after the standard headers) and get this function. In `test/epub_pipeline` it is exactly as shown. In `test/word_size_layout` the signature is `bool getGlyphInk(const int fontId, const uint32_t cp, const EpdFontFamily::Style /*style*/, int& inkLeft, int& inkWidth, int& adv) const` and the first line `adv = glyphW(fontId);`, the rest the same:
```cpp
  // Where a glyph's ink sits in its advance, as a CJK font draws its marks: an opening bracket on the right half,
  // a closing bracket, 。 and 、 on the left half, the other marks centred; anything else fills its advance.
  bool getGlyphInk(const int /*fontId*/, const uint32_t cp, const EpdFontFamily::Style style, int& inkLeft,
                   int& inkWidth, int& adv) const {
    adv = advance(cp, style);
    switch (CjkPunctuation::classOf(cp)) {
      case CjkPunctuation::Class::None:
        inkLeft = 0;
        inkWidth = adv;
        break;
      case CjkPunctuation::Class::Opening:
        inkLeft = (adv + 1) / 2;
        inkWidth = adv - inkLeft;
        break;
      case CjkPunctuation::Class::Closing:
      case CjkPunctuation::Class::FullStop:
      case CjkPunctuation::Class::Comma:
        inkLeft = 0;
        inkWidth = adv / 2;
        break;
      default:
        inkLeft = adv / 4;
        inkWidth = adv / 2;
        break;
    }
    return true;
  }
```
    In `test/epub_pipeline/GfxRenderer.h` a full-width CJK character is one 16 px em (the grid and the punctuation rules need real ems); add the header-comment line "advance(cp), a full-width CJK character (Han, kana, Hangul, CJK punctuation, full-width forms) = 16 px, one em, as a CJK font sets them", `#include <Utf8.h>`, and:
```cpp
  static int advance(const uint32_t cp, const EpdFontFamily::Style style) {
    const bool fullWidthCjk = utf8IsCjkBreakable(cp) && !(cp >= 0xFF61 && cp <= 0xFFDC);  // halfwidth forms are narrow
    const int base = fullWidthCjk ? 16 : 6 + static_cast<int>(cp % 7);
    // Style is a bitmask; only the BOLD bit affects width in this synthetic model.
    return (style & EpdFontFamily::BOLD) ? base + 1 : base;
  }
```
  - **Device renderer.** Declare `getGlyphInk` after `getTextAdvanceX` in `GfxRenderer.h` (comment: "Where codepoint `cp`'s ink sits in its advance, in pixels at the size `fontId` draws (its base scale applied, as getTextAdvanceX does): the ink starts `inkLeft` from the pen and is `inkWidth` wide. False when the font has no glyph for it. Layout places narrowed CJK punctuation by it (CjkPunctuation.h)."), and define it before `getTextAdvanceX`:
```cpp
bool GfxRenderer::getGlyphInk(const int fontId, const uint32_t cp, const EpdFontFamily::Style style, int& inkLeft,
                              int& inkWidth, int& advance) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) return false;
  const EpdGlyphRef glyph = family->getGlyph(cp, style);
  if (!glyph) return false;
  const float scale = fontBaseScale(fontId);
  inkLeft = static_cast<int>(glyph.left * scale);
  inkWidth = static_cast<int>(glyph.width * scale + 0.5f);
  advance = static_cast<int>(fp4::toPixel(glyph.advanceX) * scale + 0.5f);
  return true;
}
```
  - **`ParsedText.h`:** the members and declarations of this task's Interfaces block, `lineStartTrim_`/`lineEndTrim_` right after `wordWidths_` with the comment "How much wider (> 0) or narrower (< 0) each token is when a line starts at it (an opening CJK mark) or ends after it (any other mark) than its width mid-line, in pixels (CjkPunctuation.h). Layout scratch in step with wordWidths_: calculateWordWidths fills all three, hyphenateWordAtIndex inserts into all three."; `withEdgeTrims` inline after `calculateWordWidths`.
  - **`calculateWordWidths`** ends with `applyPunctuationWidths(renderer, fontId, out);`, defined as:
```cpp
void ParsedText::applyPunctuationWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& widths) {
  using CjkPunctuation::Class;
  const size_t count = words.size();
  lineStartTrim_.assign(count, 0);
  lineEndTrim_.assign(count, 0);

  // A mark is CJK punctuation only where its font draws it full-width, at least 4/5 of the em of 一 in the same
  // face and size: a curly quote in a Latin font is narrow. Every token is judged on its natural width before any
  // is narrowed, as a narrowed neighbour (” at 6/8 before “) would no longer pass.
  int emFont = -1;
  EpdFontFamily::Style emStyle = EpdFontFamily::REGULAR;
  float emScale = 0.0f;
  int em = 0;
  const auto fullWidth = [&](const size_t i) {
    const EpdFontFamily::Style style = static_cast<EpdFontFamily::Style>(wordStyles[i] & EpdFontFamily::BOLD_ITALIC);
    if (wordFontId(i, fontId) != emFont || style != emStyle || wordScale(i) != emScale) {
      emFont = wordFontId(i, fontId);
      emStyle = style;
      emScale = wordScale(i);
      em = measureWordWidth(renderer, emFont, "\xE4\xB8\x80", emStyle, false, emScale);  // 一
    }
    return em > 0 && widths[i] * 5 >= em * 4;
  };
  const auto singleCodepoint = [&](const size_t i) {
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    return *p == 0 ? cp : 0;
  };

  // The class of each token that is one full-width mark (addWord makes every adjustable mark a token).
  std::vector<uint8_t>& marks = markClass_;
  marks.assign(count, static_cast<uint8_t>(Class::None));
  for (size_t i = 0; i < count; ++i) {
    const uint32_t cp = singleCodepoint(i);
    const Class c = cp != 0 ? CjkPunctuation::classOf(cp) : Class::None;
    if (c != Class::None && fullWidth(i)) marks[i] = static_cast<uint8_t>(c);
  }
  // The class of token j's character next to the mark: a mark token's own, else the edge codepoint's (a curly quote
  // inside a word is Latin).
  const auto neighbourClass = [&](const size_t j, const uint32_t edge) {
    if (singleCodepoint(j) != 0) return static_cast<Class>(marks[j]);
    return CjkPunctuation::isCurlyQuote(edge) ? Class::None : CjkPunctuation::classOf(edge);
  };

  const auto table = typography_.punctuation;
  for (size_t i = 0; i < count; ++i) {
    const auto mark = static_cast<Class>(marks[i]);
    if (mark == Class::None || mark == Class::IdeographicSpace || mark == Class::AmbiguousQuote) continue;
    // An opening mark looks at the character before it, every other mark at the one after it; across a space there
    // is no neighbour.
    Class neighbour = Class::None;
    if (mark == Class::Opening) {
      if (i > 0 && !TokenBoundary::hasSpace(wordBoundaries[i])) {
        neighbour = neighbourClass(i - 1, lastCodepoint(words[i - 1]));
      }
    } else if (i + 1 < count && !TokenBoundary::hasSpace(wordBoundaries[i + 1])) {
      neighbour = neighbourClass(i + 1, firstCodepoint(words[i + 1]));
    }
    const int full = widths[i];
    const int mid = (full * CjkPunctuation::widthOf(table, mark, neighbour, false).forced + 4) / 8;
    const int edge = (full * CjkPunctuation::widthOf(table, mark, Class::None, true).forced + 4) / 8;
    widths[i] = static_cast<uint16_t>(mid);
    (mark == Class::Opening ? lineStartTrim_ : lineEndTrim_)[i] = static_cast<int16_t>(edge - mid);
  }
}
```
  - **The four breakers** measure a candidate line with its edge widths: in `computeLineBreaks` after `currlen += wordWidths[j] + gap;` add `const int lineLen = withEdgeTrims(currlen, i, j);` (comment: "The line as drawn: its first and last token at their line-edge widths (CJK punctuation, spec 5.2). It only grows with j, as a token is always wider than what a line edge takes off it.") and use `lineLen` in the `> effectivePageWidth` break test and in `remainingSpace`; in `computeHyphenatedLineBreaks` and `computeHyphenatedLineBreaksFromIndex` the fit test becomes `withEdgeTrims(lineWidth + candidateWidth, lineStart, currentIndex) <= effectivePageWidth`; in `computeSingleLineBreakNoHyphen` `withEdgeTrims(lineWidth + candidateWidth, lineStartIndex, currentIndex) <= effectivePageWidth`.
  - **`hyphenateWordAtIndex`** keeps the trims in step: after `wordWidths.insert(...)` add `lineStartTrim_.insert(lineStartTrim_.begin() + wordIndex + 1, 0);` and the same for `lineEndTrim_`.
  - **`releaseLayoutScratch`** gives the three new vectors back with the rest of the scratch: after `std::vector<uint16_t>().swap(wordWidths_);` add `std::vector<int16_t>().swap(lineStartTrim_);`, `std::vector<int16_t>().swap(lineEndTrim_);`, `std::vector<uint8_t>().swap(markClass_);`.
  - **`extractLine`**: before the word-width sum add
```cpp
  // A token's width on this line: mid-line, but the first and last at their line-edge widths (CJK punctuation).
  const auto boxWidth = [&](const size_t wordIdx) {
    int width = wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) width += lineStartTrim_[lastBreakAt];
    if (wordIdx + 1 == lineWordCount) width += lineEndTrim_[lastBreakAt + wordIdx];
    return width;
  };
```
    use `boxWidth(wordIdx)` in `lineWordWidthSum`; before the x-position loop add the ink shift:
```cpp
  // A narrowed CJK mark is drawn where its ink sits: ink in the right half of its em (an opening bracket) moves
  // left by what the mark lost, centred ink by half of it, ink in the left half stays (spec 5.2).
  const auto inkShift = [&](const size_t wordIdx, const int box) {
    const size_t i = lastBreakAt + wordIdx;
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || *p != 0 || CjkPunctuation::classOf(cp) == CjkPunctuation::Class::None) return 0;
    const int natural = measureWordWidth(renderer, wordFontId(i, fontId), words[i], wordStyles[i], false, wordScale(i));
    if (box >= natural) return 0;
    int inkLeft = 0;
    int inkWidth = 0;
    int advance = 0;
    if (!renderer.getGlyphInk(wordFontId(i, fontId), cp, wordStyles[i], inkLeft, inkWidth, advance) || advance <= 0) {
      return 0;
    }
    const int removed = natural - box;
    if (2 * inkLeft >= advance) return removed;
    if (2 * (inkLeft + inkWidth) <= advance) return 0;
    return removed / 2;
  };
```
    and in the loop: `const int box = boxWidth(wordIdx); lineXPos.push_back(static_cast<int16_t>(xpos - inkShift(wordIdx, box)));`, `int advance = box;` on the continuation path, `xpos += box + gap;` on the gap path.

- [ ] **Step 4: The rule checker.** In `CjkRulesTest.cpp`:
  - includes `"Epub/CjkPunctuation.h"`, `"Epub/TypographyProfile.h"`, `<algorithm>`, `<limits>`, `<map>`; the header comment lists the punctuation rule;
  - `DumpLine` gains `std::string shape`, `bool justified`, `bool cell`, `int y`, parsed from the `LINE`/`LN` record (`x=`, `align=`, `mult=` make the shape; `align=0` is justified; an `LN` record is a cell; `y=`). An `LN` record carries none of these fields, so a cell line's shape is empty and its y 0;
  - `Case` gains `std::string language`; `CjkRules` gets `table = TypographyProfile::forLanguage(c.language).punctuation` in `SetUp`, a `virtual uint8_t alignment() const { return 1; }` used for `profile.paragraphAlignment`, and the cache directory name ends `_justified` when `alignment() == 0`;
  - the `LineWidths` helper class before `struct Case`:
```cpp
// The width a token is laid out at, its line-edge width at the start (an opening mark) or end (any other mark) of
// its line, and how far left of that box its ink puts its draw position (spec 5.2): ParsedText's rules, restated.
class LineWidths {
 public:
  LineWidths(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile::Punctuation table)
      : renderer_(renderer), line_(line), table_(table) {}

  int box(const size_t k) const {
    const int natural = wordWidth(renderer_, line_, line_.words[k]);
    const CjkPunctuation::Class mark = markClass(k);
    if (mark == CjkPunctuation::Class::None) return natural;
    const bool opening = mark == CjkPunctuation::Class::Opening;
    const bool atEdge = opening ? k == 0 : k + 1 == line_.words.size();
    CjkPunctuation::Class neighbour = CjkPunctuation::Class::None;
    if (!atEdge) {
      neighbour = opening ? edgeClass(k - 1, lastCodepoint(line_.words[k - 1].text))
                          : edgeClass(k + 1, firstCodepoint(line_.words[k + 1].text));
    }
    return (natural * CjkPunctuation::widthOf(table_, mark, neighbour, atEdge).forced + 4) / 8;
  }

  // The left edge of token k's box: its draw position plus the shift its ink placement took off it.
  int left(const size_t k) const {
    const int removed = wordWidth(renderer_, line_, line_.words[k]) - box(k);
    switch (markClass(k)) {
      case CjkPunctuation::Class::Opening:
        return line_.words[k].x + removed;
      case CjkPunctuation::Class::Closing:
      case CjkPunctuation::Class::FullStop:
      case CjkPunctuation::Class::Comma:
      case CjkPunctuation::Class::None:
        return line_.words[k].x;
      default:
        return line_.words[k].x + removed / 2;
    }
  }

  int right(const size_t k) const { return left(k) + box(k); }

 private:
  int em(const size_t k) const {
    DumpWord one = line_.words[k];
    one.text = "\xE4\xB8\x80";  // 一
    return wordWidth(renderer_, line_, one);
  }
  bool fullWidth(const size_t k) const { return wordWidth(renderer_, line_, line_.words[k]) * 5 >= em(k) * 4; }
  CjkPunctuation::Class edgeClass(const size_t k, const uint32_t cp) const {
    if (CjkPunctuation::isCurlyQuote(cp) && !fullWidth(k)) return CjkPunctuation::Class::None;
    return CjkPunctuation::classOf(cp);
  }
  // The class of token k when it is one full-width mark whose width the table sets, else None.
  CjkPunctuation::Class markClass(const size_t k) const {
    const std::string& text = line_.words[k].text;
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || *p != 0) return CjkPunctuation::Class::None;
    const CjkPunctuation::Class c = edgeClass(k, cp);
    if (c == CjkPunctuation::Class::IdeographicSpace || c == CjkPunctuation::Class::AmbiguousQuote) {
      return CjkPunctuation::Class::None;
    }
    return c != CjkPunctuation::Class::None && fullWidth(k) ? c : CjkPunctuation::Class::None;
  }

  const GfxRenderer& renderer_;
  const DumpLine& line_;
  TypographyProfile::Punctuation table_;
};
```
  - `NoSpaceBetweenUnspacedCjkCharacters` is replaced by `UnspacedCjkSitsWhereItsWidthsPutIt` (and `hasCjkGap`, used in Task 5):
```cpp
// Each character of unspaced CJK text sits where the widths before it put it: no space where the source broke a
// line, where the parser flushed a long run or across an inline element, and every CJK mark at the width its
// book's punctuation table gives it beside its neighbours (spec 5.2). A justified line (table cells keep their
// own alignment) may stretch the gap, never overlap it.
TEST_P(CjkRules, UnspacedCjkSitsWhereItsWidthsPutIt) {
  for (const auto& line : lines) {
    const LineWidths widths(renderer, line, table);
    for (size_t i = 0; i + 1 < line.words.size(); ++i) {
      const DumpWord& left = line.words[i];
      const DumpWord& right = line.words[i + 1];
      if (!utf8IsUnspacedCjk(lastCodepoint(left.text)) || !utf8IsUnspacedCjk(firstCodepoint(right.text))) continue;
      const int gap = widths.left(i + 1) - widths.right(i);
      if (line.justified || line.cell) {
        EXPECT_GE(gap, 0) << "\"" << right.text << "\" overlaps \"" << left.text << "\" in: " << lineText(line);
      } else {
        EXPECT_EQ(gap, 0) << "\"" << right.text << "\" is not where \"" << left.text
                          << "\" ends, in: " << lineText(line);
      }
    }
  }
}

// The line has a breakable gap between two characters of unspaced CJK text, the gaps a justified CJK line stretches.
bool hasCjkGap(const DumpLine& line) {
  for (size_t i = 0; i + 1 < line.words.size(); ++i) {
    const uint32_t left = lastCodepoint(line.words[i].text);
    const uint32_t right = firstCodepoint(line.words[i + 1].text);
    if (utf8IsUnspacedCjk(left) && utf8IsUnspacedCjk(right) && CjkLineBreak::isGapBetween(left, right)) return true;
  }
  return false;
}
```
  - the books get their languages, with the new Simplified Chinese book (the justified suite is instantiated in Task 5):
```cpp
std::vector<Case> cjkBooks() {
  std::vector<Case> cases;
  for (const auto& [book, language] : std::vector<std::pair<const char*, const char*>>{
           {"test_cjk_line_breaking", "ja"},
           {"test_cjk_ja_soseki", "ja"},
           {"test_cjk_ko_hyeon", "ko"},
           {"test_cjk_zh_honglou", "zh-Hant"},
           {"test_cjk_zh_punctuation", "zh-Hans"}}) {
    const std::string path = (fs::path(CORPUS_DIR) / (std::string(book) + ".epub")).string();
    cases.push_back(Case{path, language, false});
    cases.push_back(Case{path, language, true});
  }
  return cases;
}

std::string caseName(const testing::TestParamInfo<Case>& info) {
  return fs::path(info.param.epub).stem().string() + (info.param.hyphenation ? "_hyph" : "");
}

INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkRules, testing::ValuesIn(cjkBooks()), caseName);
```
  - **The Simplified Chinese book.** The Hong Lou Meng excerpt is already `zh-Hant` (spec 7 asked for a Traditional Chinese excerpt; it exists), so the missing table is Simplified Chinese. Its sentences are written for the fixture, not taken from a book:
```python
#!/usr/bin/env python3
"""Build test_cjk_zh_punctuation.epub: a Simplified Chinese book (dc:language zh-Hans) for the punctuation
widths of the Simplified Chinese table (CjkRulesTest, spec section 5.2).

The sentences are written for this fixture. They put the marks the table adjusts in the places it adjusts them:
runs of marks (。” ！” ？” ）。), a colon before a quotation, the enumeration comma, book title marks, brackets,
the two-em dash, marks at line ends, digits and Latin inside Chinese text, and an ideographic space opening a
paragraph.

Regenerate with:  python test/epubs/make_test_cjk_zh_punctuation.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "test_cjk_zh_punctuation.epub"
LANG = "zh-Hans"

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h1 { margin: 0 0 1em 0; text-indent: 0; font-size: 1.2em }
"""

PARAGRAPHS = [
    "今天早上下了一场小雨，街上的人不多。我带着伞出门，走到车站的时候，雨已经停了。"
    "车站旁边有一家小店，店主正在把桌子搬到门外：“天晴了，今天会有很多客人吧。”",
    "书架上放着三本书：《春天的花园》、《海边的小屋》和《星期六》。我最喜欢第二本——它讲的是"
    "一个渔夫和他的狗。每次读到最后一页，我都会想起小时候住过的那个村子。",
    "他问我：“你明天有空吗？”我说：“有空。”于是我们约好上午十点在图书馆门口见面"
    "（如果下雨，就改在旁边的咖啡店）。",
    "这一段用来测试连续的标点：“好！”“真的吗？”“当然。”——大家都笑了。"
    "（这里的括号后面紧跟着句号。）然后又是一句很长的话，让标点落在行尾。",
    "价格从12元涨到了15元，涨幅是25%。我们用Python写了一个小程序，统计了2020年到2024年的数据，"
    "结果和预想的差不多。",
    "　段首的全角空格保持全宽。这一段的文字也比较长，用来让逗号、顿号和句号出现在行尾，"
    "看看它们在行尾是不是只占半个字的宽度；分号也一样。",
]

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>
"""


def main():
    title = "标点测试"
    body = "\n".join(f"  <p>{p}</p>" for p in PARAGRAPHS)
    chapter = CHAPTER.format(lang=LANG, title=title, body=body)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        f"    <dc:language>{LANG}</dc:language>\n"
        f'    <dc:identifier id="bookid">{NAME}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
        "</package>\n"
    )
    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )
    out = os.path.join(HERE, NAME)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", chapter)
    print("wrote", out)


if __name__ == "__main__":
    main()
```
    Run `python test/epubs/make_test_cjk_zh_punctuation.py` and commit the `.epub`.

- [ ] **Step 5: Run.** `CjkLayoutTest.exe` — Expected: all PASS. Regenerate the goldens (`UPDATE_GOLDENS=1 ... --gtest_filter='SyntheticCorpus/*MatchesGolden*'`) — Expected: only `test_cjk_*` goldens changed, plus the new `test_cjk_zh_punctuation.golden.txt`. Full pipeline run — Expected: all PASS, including `CjkCorpus/CjkRules.*` on all five books.

- [ ] **Step 6: Commit.** `git commit -m "feat(epub): CJK punctuation at its table widths, at line edges too, placed by its ink"`

---

### Task 5: Justified CJK lines end flush

**Files:** Modify `lib/Epub/Epub/ParsedText.cpp` (`extractLine`), `test/cjk_layout/CjkTypographyTest.cpp`, `test/epub_pipeline/CjkRulesTest.cpp`.

**Interfaces:** Consumes `boxWidth` (Task 4). Produces nothing new outside `extractLine`.

- [ ] **Step 1: Write the failing tests:**
```cpp
TEST(CjkJustification, AJustifiedCjkLineEndsFlush) {
  // 4 characters on a 47 px line: 7 px over 3 gaps, 2 px each and the last pixel to one of them.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  text.addWord("一二三四五六七八", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos.back() + 10, 47);
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 24, 37}));
}
TEST(CjkJustification, LatinLinesKeepTheirRounding) {
  // aa bb cc on 81 px: 11 px over 2 word gaps, 5 each; the last pixel stays at the end as before.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  for (const char* word : {"aa", "bb", "cc", "dd"}) text.addWord(word, EpdFontFamily::REGULAR);
  const auto lines = layout(text, 81);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (X{0, 30, 60}));
}
```
  and in `CjkRulesTest.cpp` the justified suite, as in Task 6's listing below but without the `if (profile.wholeEmGrid) { ... }` block: `class CjkJustifiedRules : public CjkRules { protected: uint8_t alignment() const override { return 0; } };` after `CjkRules`, the test `JustifiedLinesEndFlushOnAWholeEmGrid`, and `INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkJustifiedRules, testing::ValuesIn(cjkBooks()), caseName);`.
- [ ] **Step 2: Run to see them fail.** Expected: `AJustifiedCjkLineEndsFlush` FAILS (`{0, 12, 24, 36}`), the justified suite FAILS "not flush" on the ja and zh books.
- [ ] **Step 3: Implement.** In `extractLine` note CJK gaps while summing: declare `bool cjkGapOnLine = false;` with the other sums, and inside `if (wordIdx > 0 && !boundary.continues) {`, right after `if (!beforeClosing) actualGapCount++;`, add `if (boundary.noSpaceBefore) cjkGapOnLine = true;`. Replace `justifyExtra`'s definition with
```cpp
  const bool justified = blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1;
  const int justifyExtra =
      justified ? std::min(spareSpace / static_cast<int>(actualGapCount), effectivePageWidth / 8) : 0;
  // A line with CJK gaps ends flush: what the even share leaves over goes 1 px each to evenly spaced gaps
  // (spec 5.1). Latin lines keep the old rounding, so their layout does not move.
  const int justifyRemainder = justified && cjkGapOnLine && justifyExtra < effectivePageWidth / 8 && spareSpace > 0
                                   ? spareSpace - justifyExtra * static_cast<int>(actualGapCount)
                                   : 0;
  int stretchedGaps = 0;
  const auto gapExtra = [&]() {
    const int g = stretchedGaps++;
    const int gaps = static_cast<int>(actualGapCount);
    if (gaps == 0) return justifyExtra;
    return justifyExtra + ((g + 1) * justifyRemainder / gaps - g * justifyRemainder / gaps);
  };
```
  and use `gapExtra()` where `justifyExtra` was added (the non-breaking-space path and the word-gap path).
- [ ] **Step 4: Run.** Expected: both tests PASS; the justified suite PASSES on every book (skipped for Korean, which justifies at word spaces); Latin goldens unchanged, CJK goldens regenerated (`git status`). `test_cjk_ko_hyeon` moves as well: a Korean line with Han characters on it has CJK gaps, so it ends flush too (the rule is per line, not per language).
- [ ] **Step 5: Commit.** `git commit -m "feat(epub): justified CJK lines end flush"`

---

### Task 6: The whole-em grid for Chinese and Japanese books

**Files:** Modify `lib/Epub/Epub/ParsedText.h` (`int16_t gridInset_ = 0;` after the trims, comment "How far the block's lines sit in from the left: half of what rounding the width down to whole ems took (the CJK grid, spec section 5.5). Set by each layout pass."), `lib/Epub/Epub/ParsedText.cpp`, `test/cjk_layout/CjkTypographyTest.cpp`, `test/epub_pipeline/CjkRulesTest.cpp`.

**Interfaces:** Consumes `typography_.wholeEmGrid` (Task 1).

- [ ] **Step 1: Write the failing tests:**
```cpp
TEST(CjkGrid, AJapaneseBookRoundsTheWidthDownToWholeEmsAndCentresIt) {
  // 47 px holds 4 ems of 10 px: the lines are 40 px wide, 3 px in from the left.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三", "四"}, {"五"}}));
  EXPECT_EQ(lines[0].xpos, (X{3, 13, 23, 33}));
  EXPECT_EQ(lines[1].xpos, (X{3}));
}
TEST(CjkGrid, AnIndentRoundsDownToWholeEmsAndNoIndentStaysNone) {
  for (const auto& [indent, firstX] : std::vector<std::pair<int16_t, int16_t>>{{15, 13}, {5, 13}, {0, 3}}) {
    ParsedText text(false, false, aligned(CssTextAlign::Left, indent));
    text.setTypography(TypographyProfile::forLanguage("zh"));
    text.addWord("一二三", EpdFontFamily::REGULAR);
    EXPECT_EQ(layout(text, 47)[0].xpos.front(), firstX) << "indent " << indent;
  }
}
TEST(CjkGrid, OtherBooksHaveNoGrid) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkGrid, AHeadingsGridIsInItsOwnEms) {
  // A block at 1.2 times the body size has 12 px ems: 47 px holds 3 of them, 5 px in from the left.
  BlockStyle heading = aligned(CssTextAlign::Left);
  heading.fontSizeMultiplier = 1.2f;
  ParsedText text(false, false, heading);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三"}, {"四"}}));
  EXPECT_EQ(lines[0].xpos, (X{5, 17, 29}));
}
TEST(CjkGrid, ABlockBesideAFloatKeepsItsOwnWidth) {
  BlockStyle beside = aligned(CssTextAlign::Left);
  beside.floatZoneCount = 1;
  beside.floatZones[0] = {0, 0, 0, false};  // a zone no line meets: the block is beside a float all the same
  ParsedText text(false, false, beside);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}
```
  and complete the justified rule with the grid check:
```cpp
// A justified line with CJK gaps that is not its paragraph's last ends flush (spec 5.1), all at one right edge,
// and in a Chinese or Japanese book the text width is a whole number of ems (spec 5.5). Lines with one LINE record
// shape (x, alignment, size) share a text width; the shape's left edge is where its leftmost line starts. A line
// is not its paragraph's last when the next line of its flow has the same shape, starts at that left edge (not
// indented) and follows without a margin. Lines justified at word spaces (Latin, Korean) keep their old rounding
// and are not checked.
TEST_P(CjkJustifiedRules, JustifiedLinesEndFlushOnAWholeEmGrid) {
  const TypographyProfile profile = TypographyProfile::forLanguage(GetParam().language);
  if (profile.justify != TypographyProfile::Justify::BetweenCharacters) GTEST_SKIP() << "justified at word spaces";

  std::map<std::string, int> leftEdge;  // shape -> where its leftmost line starts
  std::map<std::string, int> lineStep;  // shape -> its smallest step down the page
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].words.empty()) continue;
    const int left = LineWidths(renderer, lines[i], table).left(0);
    const auto [at, inserted] = leftEdge.emplace(lines[i].shape, left);
    if (!inserted) at->second = std::min(at->second, left);
    if (i > 0 && lines[i].followsLine && lines[i - 1].shape == lines[i].shape && lines[i].y > lines[i - 1].y) {
      const auto [step, fresh] = lineStep.emplace(lines[i].shape, lines[i].y - lines[i - 1].y);
      if (!fresh) step->second = std::min(step->second, lines[i].y - lines[i - 1].y);
    }
  }

  std::map<std::string, std::vector<std::pair<int, size_t>>> rightEdges;  // shape -> (right edge, line)
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    const DumpLine& line = lines[i];
    const DumpLine& next = lines[i + 1];
    if (line.words.empty() || next.words.empty() || !line.justified || !hasCjkGap(line)) continue;
    if (!next.followsLine || next.shape != line.shape) continue;
    if (LineWidths(renderer, next, table).left(0) != leftEdge[line.shape]) continue;  // the next paragraph's start
    if (next.y > line.y && next.y - line.y > lineStep[line.shape]) continue;          // a margin: a new paragraph
    rightEdges[line.shape].emplace_back(LineWidths(renderer, line, table).right(line.words.size() - 1), i);
  }
  ASSERT_FALSE(rightEdges.empty()) << "no justified CJK line to check";

  for (const auto& [shape, edges] : rightEdges) {
    int right = 0;
    for (const auto& edge : edges) right = std::max(right, edge.first);
    for (const auto& [edge, i] : edges) EXPECT_EQ(edge, right) << "not flush: " << lineText(lines[i]);
    if (profile.wholeEmGrid) {
      DumpWord one;
      one.text = "\xE4\xB8\x80";  // 一
      const int em = ::wordWidth(renderer, lines[edges.front().second], one);
      EXPECT_EQ((right - leftEdge[shape]) % em, 0)
          << "text width " << right - leftEdge[shape] << " is not whole ems of " << em << " (" << shape << ")";
    }
  }
}
```
- [ ] **Step 2: Run to see them fail.** Expected: the grid tests FAIL (x starts at 0), the justified suite FAILS "is not whole ems" on the ja and zh books.
- [ ] **Step 3: Implement.** In `layoutAndExtractLines` make `pageWidth` non-const and, after `firstLineIndent` is computed and before `calculateWordWidths`, add:
```cpp
  // The CJK grid (spec 5.5): a Chinese or Japanese book's text width rounds down to whole ems of the block's font,
  // half the cut on each side, so pure CJK lines fill exactly and characters line up in columns. A first-line
  // indent rounds down to whole ems, at least one; a block with none keeps none. Floats keep their own width.
  gridInset_ = 0;
  if (typography_.wholeEmGrid && blockStyle.floatZoneCount == 0) {
    const int em = measureWordWidth(renderer, fontId, "\xE4\xB8\x80", EpdFontFamily::REGULAR, false,
                                    blockStyle.fontSizeMultiplier);
    if (em > 0 && pageWidth >= em) {
      const int gridWidth = pageWidth - pageWidth % em;
      gridInset_ = static_cast<int16_t>((pageWidth - gridWidth) / 2);
      pageWidth = gridWidth;
      if (firstLineIndent > 0) firstLineIndent = std::max(em, firstLineIndent - firstLineIndent % em);
    }
  }
```
  In `extractLine` the first x is `lineIndent + gridInset_`, and the right and centre alignments add `gridInset_` to theirs.
- [ ] **Step 4: Run.** Expected: all PASS; CJK goldens regenerated, Latin goldens unchanged.
- [ ] **Step 5: Commit.** `git commit -m "feat(epub): Chinese and Japanese text on a whole-em grid"`

---

### Task 7: Device validation, budgets, docs

**Files:** `docs/future_work/cjk.md`; no code unless the device finds a fault.

- [ ] **Step 1: Full host suite.** `cmake --build build/test -j 8 -- -k 0 && cd build/test && ctest -j 8` — Expected: all PASS but the four Windows failures named in the Global Constraints.
- [ ] **Step 2: Firmware.** `pio run -e default`, `-e x4pro`, `-e lilygo_t5s3` — Expected: each SUCCESS.
- [ ] **Step 3: Device (X3 or X4, NotoSansCJK installed as an SD font).** With `CMD:SET:fontFamily=NotoSansCJK` and the books `test_cjk_ja_soseki`, `test_cjk_zh_honglou`, `test_cjk_zh_punctuation` copied to the card: screenshots (`tools/devctl.py ... shot:`) of a page of each, checked for half-width 。」 runs, line-end marks, an opening bracket at a line start drawn flush with the margin, flush right edges, and characters in columns. Also open `test_cjk_ja_soseki` with a built-in Latin reader font (Bookerly): it must lay out as before (no narrowing, no grid). Restore the device's settings afterwards.
- [ ] **Step 4: Budgets (spec section 6).** On the device, with `CMD:STATE` heap figures and the section-build log lines, compare a cold section build of `test_cjk_ja_soseki` and `test_cjk_zh_honglou` against master (A2): build time at most +10 %, heap low-water mark at most 5 % lower, page time unchanged within noise. Record the figures in the PR.
- [ ] **Step 5: Docs.** In `docs/future_work/cjk.md` move "punctuation widths, flush justification, grid" from the A3 section to "Already shipped" (with this PR's number), and leave push-in and the CJK–Latin gap as A3b.
- [ ] **Step 6: clang-format 21** (`bin/clang-format-fix` with a version-21 binary on the PATH) and commit any reformatting: `git commit -m "style: clang-format 21"`.
