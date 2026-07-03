// Smoke test for DictHtmlRenderer.
// Reads the entries from test/dictionaries/html-definitions/ and passes each full
// entry to renderer.render() — exactly as on-device code would — checking the
// complete span output against expected values. Also verifies that the streaming
// path (renderFromFileStreaming, reading the .dict file via the HalStorage stub)
// produces identical spans, plus boundary cases and the IPA utility functions.
// DICT_HTML_RENDERER_TRACK_UNKNOWN and DICT_HTML_TEST_DATA are defined by CMakeLists.txt.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/DictHtmlRenderer/DictHtmlRenderer.h"
#include "src/util/DictFontUtils.h"

// ---------------------------------------------------------------------------
// Helper: read binary file
// ---------------------------------------------------------------------------
static std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------------------
// StarDict .idx parser
// ---------------------------------------------------------------------------
struct DictEntry {
  std::string word;
  uint32_t offset;
  uint32_t length;
};

static std::vector<DictEntry> parseIdx(const std::string& idx) {
  std::vector<DictEntry> entries;
  size_t pos = 0;
  while (pos < idx.size()) {
    size_t nul = idx.find('\0', pos);
    if (nul == std::string::npos) break;
    if (nul + 9 > idx.size()) break;
    DictEntry e;
    e.word = idx.substr(pos, nul - pos);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(idx.data() + nul + 1);
    e.offset = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    e.length = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    entries.push_back(e);
    pos = nul + 9;
  }
  return entries;
}

// ---------------------------------------------------------------------------
// Span comparison helpers
// ---------------------------------------------------------------------------
struct ExpectedSpan {
  const char* text;
  bool bold;
  bool italic;
  bool underline;
  bool strikethrough;
  bool isListItem;
  bool newlineBefore;
  uint8_t indentLevel;
};

// Shorthand constructor — keeps expected tables readable
static ExpectedSpan S(const char* t, bool nl = false, bool bold = false, bool italic = false, bool ul = false,
                      bool strike = false, bool li = false, uint8_t indent = 0) {
  return {t, bold, italic, ul, strike, li, nl, indent};
}

static bool spanMatches(const StyledSpan& got, const ExpectedSpan& exp) {
  if (!got.text || strcmp(got.text, exp.text) != 0) return false;
  if (got.bold != exp.bold) return false;
  if (got.italic != exp.italic) return false;
  if (got.underline != exp.underline) return false;
  if (got.strikethrough != exp.strikethrough) return false;
  if (got.isListItem != exp.isListItem) return false;
  if (got.newlineBefore != exp.newlineBefore) return false;
  if (got.indentLevel != exp.indentLevel) return false;
  return true;
}

// A span copied out of the renderer (text owned), for comparing batch render()
// output against the streaming renderFromFileStreaming() output.
struct CollectedSpan {
  std::string text;
  bool bold, italic, underline, strikethrough, isListItem, newlineBefore;
  uint8_t indentLevel;
  bool operator==(const CollectedSpan& o) const {
    return text == o.text && bold == o.bold && italic == o.italic && underline == o.underline &&
           strikethrough == o.strikethrough && isListItem == o.isListItem && newlineBefore == o.newlineBefore &&
           indentLevel == o.indentLevel;
  }
};

static CollectedSpan collect(const StyledSpan& s) {
  return {s.text ? s.text : "", s.bold,       s.italic,        s.underline,
          s.strikethrough,      s.isListItem, s.newlineBefore, s.indentLevel};
}

static void collectSink(void* ctx, const StyledSpan& s) {
  static_cast<std::vector<CollectedSpan>*>(ctx)->push_back(collect(s));
}

// ---------------------------------------------------------------------------
// Expected outputs — full entry content including description paragraphs.
//
// Each entry has:
//   description paragraphs (plain <p> text)
//   <p>----------</p>        (delimiter — rendered as span "----------")
//   test HTML section
//   <p>----------</p>        (closing delimiter)
// ---------------------------------------------------------------------------

// BlazeSilent → AbbrExpand
static const std::vector<ExpectedSpan> kAbbrExpand = {
    S("Three abbreviations should expand inline with their full title in parentheses.", true),
    S("Case 1: single-word title. Expected: c. (circa)", true),
    S("Case 2: multi-word title containing a space. Expected: AD (anno Domini)", true),
    S("Case 3: italic element inside abbr. Expected: f. (filius) with f. rendered italic.", true),
    S("----------", true),
    S("Abbreviation one: ", true),
    S("c."),
    S(" (circa)"),
    S("Abbreviation two: ", true),
    S("AD"),
    S(" (anno Domini)"),
    S("Abbreviation three: ", true),
    S("f.", false, false, /*italic=*/true),
    S(" (filius)"),
    S("----------", true),
};

// ClearSvg → BlockStrip
static const std::vector<ExpectedSpan> kBlockStrip = {
    S("Nine block tags and all their children should be stripped entirely.", true),
    S("Nothing should appear between the two rules below.", true),
    S("Tags tested: hiero (with nested table/tr/td/img), svg (with defs/g/path/rect/use), math (with sup), gallery, "
      "nowiki, poem, ref (lowercase), REF (uppercase), img (standalone).",
      true),
    S("----------", true),
    S("----------", true),
};

// DarkMath → BlockStruct
static const std::vector<ExpectedSpan> kBlockStruct = {
    S("Block structure elements. Expected output in order:", true),
    S("Two separate paragraphs. Two div blocks. Three lines separated by br. An indented blockquote. A numbered list "
      "(first, second, third). A bulleted list (alpha, beta, gamma). Four bold headings (Heading One through Heading "
      "Four).",
      true),
    S("----------", true),
    S("First paragraph.", true),
    S("Second paragraph.", true),
    S("First div block.", true),
    S("Second div block.", true),
    S("Line one.", true),
    S("Line two.", true),
    S("Line three.", true),
    S("indented passage", true, false, false, false, false, false, /*indent=*/1),
    S("first", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("second", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("third", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("alpha", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("beta", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("gamma", true, false, false, false, false, /*li=*/true, /*indent=*/1),
    S("Heading One", true, /*bold=*/true),
    S("Heading Two", true, /*bold=*/true),
    S("Heading Three", true, /*bold=*/true),
    S("Heading Four", true, /*bold=*/true),
    S("----------", true),
};

// EmptyGallery → FormatTags
static const std::vector<ExpectedSpan> kFormatTags = {
    S("Inline formatting tags. Expected words with their styles:", true),
    S("bold (b), bold (strong), italic (i), italic (em), underline (u), strikethrough (s), subscript 2 in H2O (sub), "
      "superscript 2 in x2 (sup), code style (code), code style (tt), small size (small), big size (big), italic var "
      "(var).",
      true),
    S("Nested: bold-italic (b+i). Triple: bold-italic-underline (b+i+u).", true),
    S("----------", true),
    // FORMAT_SMALL (sub, sup, small, big) flushes pending text on open/close,
    // so each tag boundary produces separate spans.
    // FORMAT_CODE (code, tt) flushes and applies bold.
    S("bold", true, /*bold=*/true),
    S(" "),
    S("bold", false, true),
    S(" "),
    S("italic", false, false, /*italic=*/true),
    S(" "),
    S("italic", false, false, true),
    S(" "),
    S("underline", false, false, false, /*ul=*/true),
    S(" "),
    S("strike", false, false, false, false, /*strike=*/true),
    S(" H"),
    S("2"),
    S("O x"),
    S("2"),
    S(" "),
    S("printf", false, /*bold=*/true),
    S(" "),
    S("mono", false, true),
    S(" "),
    S("small"),
    S(" "),
    S("big"),
    S(" "),
    S("count", false, false, true),
    S("Nested: ", true),
    S("bold-italic", false, true, /*italic=*/true),
    S(" "),
    S("bold-italic-underline", false, true, true, /*ul=*/true),
    S("----------", true),
};

// FrostNowiki → StripKeep (has intentional unknown tags)
static const std::vector<ExpectedSpan> kStripKeep = {
    S("Four cases where the tag is stripped but its text content is kept.", true),
    S("Case 1: span tag stripped, text kept. Expected: visible span text", true),
    S("Case 2: single unknown tag stripped, text kept. Expected: visible unknown text", true),
    S("Case 3: nested unknown tags both stripped, innermost text kept. Expected: visible nested text", true),
    S("Case 4: anchor tag stripped, text kept. Expected: visible anchor text", true),
    S("----------", true),
    S("visible span text", true),
    S("visible unknown text", true),
    S("visible nested text", true),
    S("visible anchor text", true),
    S("----------", true),
};

// GlowPoem → WikiAnnot
static const std::vector<ExpectedSpan> kWikiAnnot = {
    S("Eight wikitext annotation tags. Each is a self-closing tag of the form XX:YY where the text to render is the "
      "suffix YY (the part after the colon).",
      true),
    S("A ninth case uses a tag with body text. Body must be suppressed; only the suffix renders. Expected: value",
      true),
    S("Expected inline text in order: four, oikos, la, sharp, noun, Grek, ameba, female", true),
    S("----------", true),
    S("count: ", true),
    S("four"),
    S(" origin: "),
    S("oikos"),
    S(" language: "),
    S("la"),
    S(" meaning: "),
    S("sharp"),
    S(" part of speech: "),
    S("noun"),
    S(" script: "),
    S("Grek"),
    S(" alternate: "),
    S("ameba"),
    S(" identifier: "),
    S("female"),
    S("body suppressed: ", true),
    S("value"),
    S("----------", true),
};

// HazeEntity → HtmlEntities
static const std::vector<ExpectedSpan> kHtmlEntities = {
    S("HTML named entities resolved to UTF-8. Expected: brackets, space, dash, zero-width marks dropped, unknown "
      "entity dropped.",
      true),
    S("----------", true),
    S("Brackets: [enclosed]", true),
    S("Non-breaking space: left right", true),
    S("En dash: 1939\xE2\x80\x93"
      "1945",
      true),
    S("Zero-width: before", true),
    S("Unknown: before", true),
    S("----------", true),
};

// ---------------------------------------------------------------------------
// Test fixture — loads the dictionary once per suite; fresh renderer per test.
// ---------------------------------------------------------------------------

class DictHtmlRendererTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    dictPath_ = std::string(DICT_HTML_TEST_DATA) + "/html-definitions.dict";
    std::string idxPath = std::string(DICT_HTML_TEST_DATA) + "/html-definitions.idx";
    idxData_ = readFile(idxPath);
    dictData_ = readFile(dictPath_);
    ASSERT_FALSE(idxData_.empty()) << "Could not read " << idxPath;
    ASSERT_FALSE(dictData_.empty()) << "Could not read " << dictPath_;
    entries_ = parseIdx(idxData_);
    ASSERT_FALSE(entries_.empty()) << "No entries found in idx";
  }

 protected:
  // Fresh renderer per test (avoids cross-test state pollution).
  DictHtmlRenderer renderer_;

  std::string getEntryContent(const char* word) {
    for (const auto& e : entries_) {
      if (e.word == word) {
        if (e.offset + e.length > dictData_.size()) return {};
        return dictData_.substr(e.offset, e.length);
      }
    }
    return {};
  }

  const DictEntry* findEntry(const char* word) {
    for (const auto& e : entries_) {
      if (e.word == word) return &e;
    }
    return nullptr;
  }

  // Assert that renderer.render() of the named entry's content matches expected spans.
  void checkEntry(const char* word, const std::vector<ExpectedSpan>& expected, bool expectUnknownTags = false) {
    SCOPED_TRACE(word);
    std::string content = getEntryContent(word);
    ASSERT_FALSE(content.empty()) << "Entry '" << word << "' not found or out of bounds";

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
    renderer_.currentEntryName = word;
    renderer_.unknownTagCount = 0;
#endif

    const auto& spans = renderer_.render(content.c_str(), static_cast<int>(content.size()));

    ASSERT_EQ(spans.size(), expected.size()) << "span count mismatch";
    for (int i = 0; i < static_cast<int>(expected.size()); i++) {
      EXPECT_TRUE(spanMatches(spans[i], expected[i]))
          << "span[" << i << "] mismatch: got \"" << (spans[i].text ? spans[i].text : "(null)") << "\", expected \""
          << expected[i].text << "\"";
    }

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
    if (!expectUnknownTags) {
      EXPECT_EQ(renderer_.unknownTagCount, 0) << "unexpected unknown tags";
    }
#endif
  }

  static std::string idxData_;
  static std::string dictData_;
  static std::vector<DictEntry> entries_;
  static std::string dictPath_;
};

std::string DictHtmlRendererTest::idxData_;
std::string DictHtmlRendererTest::dictData_;
std::vector<DictEntry> DictHtmlRendererTest::entries_;
std::string DictHtmlRendererTest::dictPath_;

// ---------------------------------------------------------------------------
// Entry tests (one per dictionary entry)
// ---------------------------------------------------------------------------

TEST_F(DictHtmlRendererTest, BlazeSilent_AbbrExpand) { checkEntry("BlazeSilent", kAbbrExpand); }

TEST_F(DictHtmlRendererTest, ClearSvg_BlockStrip) { checkEntry("ClearSvg", kBlockStrip); }

TEST_F(DictHtmlRendererTest, DarkMath_BlockStruct) { checkEntry("DarkMath", kBlockStruct); }

TEST_F(DictHtmlRendererTest, EmptyGallery_FormatTags) { checkEntry("EmptyGallery", kFormatTags); }

TEST_F(DictHtmlRendererTest, FrostNowiki_StripKeep) {
  checkEntry("FrostNowiki", kStripKeep, /*expectUnknownTags=*/true);
}

TEST_F(DictHtmlRendererTest, GlowPoem_WikiAnnot) { checkEntry("GlowPoem", kWikiAnnot); }

TEST_F(DictHtmlRendererTest, HazeEntity_HtmlEntities) { checkEntry("HazeEntity", kHtmlEntities); }

// ---------------------------------------------------------------------------
// Group D: streaming parity — renderFromFileStreaming() must produce exactly the
// same spans as the batch render() for every entry. Exercises the Stage 2b
// streaming span-sink path (reads the .dict file via the HalStorage stub; never
// materializes the whole-definition textBuf/spans vector).
// ---------------------------------------------------------------------------

TEST_F(DictHtmlRendererTest, StreamingParity) {
  struct TestCase {
    const char* word;
    bool expectUnknownTags;
  };
  const TestCase cases[] = {
      {"BlazeSilent", false}, {"ClearSvg", false}, {"DarkMath", false},   {"EmptyGallery", false},
      {"FrostNowiki", true},  {"GlowPoem", false}, {"HazeEntity", false},
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.word);
    const DictEntry* found = findEntry(tc.word);
    ASSERT_NE(found, nullptr) << "entry not found: " << tc.word;
    ASSERT_LE(found->offset + found->length, dictData_.size()) << "entry out of bounds";

    const std::string raw = dictData_.substr(found->offset, found->length);

    // Batch spans — copied out, since render() reuses its buffer on the next call.
    std::vector<CollectedSpan> batch;
    for (const auto& s : renderer_.render(raw.c_str(), static_cast<int>(raw.size()))) batch.push_back(collect(s));

    // Streamed spans read from the .dict file.
    std::vector<CollectedSpan> stream;
    DictHtmlRenderer::SpanSink sink{&stream, &collectSink};
    renderer_.renderFromFileStreaming(dictPath_.c_str(), found->offset, found->length, sink);

    EXPECT_EQ(batch.size(), stream.size()) << "batch/stream span count mismatch for: " << tc.word;
    const size_t n = std::min(batch.size(), stream.size());
    for (size_t i = 0; i < n; i++) {
      EXPECT_EQ(batch[i], stream[i]) << "span[" << i << "] mismatch for: " << tc.word;
    }
  }
}

// ---------------------------------------------------------------------------
// B1: parseError — malformed XML produces partial output
// The renderer wraps input in <_root>...</_root>. Unclosed tags cause a parse
// error, but partial spans accumulated before the error are still returned.
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, ParseError_PartialOutput) {
  const auto& badSpans = renderer_.render("<p>unclosed", 11);
  ASSERT_EQ(badSpans.size(), 1u) << "malformed XML: expected 1 partial span";
  EXPECT_TRUE(badSpans[0].text && strcmp(badSpans[0].text, "unclosed") == 0) << "partial span text is 'unclosed'";
}

// ---------------------------------------------------------------------------
// B2: Large input — 100 paragraphs of 90 chars each (9000 bytes)
// Dynamic buffers handle all 100 paragraphs; every span must have valid text.
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, LargeInput_100Paragraphs) {
  std::string bigHtml;
  bigHtml.reserve(100 * 96);
  for (int i = 0; i < 100; i++) {
    bigHtml += "<p>";
    bigHtml.append(90, 'A');
    bigHtml += "</p>";
  }
  const auto& bigSpans = renderer_.render(bigHtml.c_str(), static_cast<int>(bigHtml.size()));
  ASSERT_EQ(bigSpans.size(), 100u) << "expected 100 spans";
  for (int i = 0; i < 100; i++) {
    EXPECT_NE(bigSpans[i].text, nullptr) << "span[" << i << "].text is null";
  }
}

// ---------------------------------------------------------------------------
// B3: Long single paragraph — 600 chars in one <p>
// Dynamic pendingText has no fixed limit; entire text emitted as one span.
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, LongParagraph_600Chars) {
  std::string html = "<p>";
  html.append(600, 'B');
  html += "</p>";
  const auto& spans = renderer_.render(html.c_str(), static_cast<int>(html.size()));
  ASSERT_EQ(spans.size(), 1u) << "expected 1 span";
  ASSERT_NE(spans[0].text, nullptr) << "span text is null";
  EXPECT_EQ(strlen(spans[0].text), 600u) << "span length is 600";
}

// ---------------------------------------------------------------------------
// B4: Deep nesting — 35 nested <i> tags (dynamic tag stack, no fixed limit)
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, DeepNesting_35NestedItalics) {
  std::string html;
  for (int i = 0; i < 35; i++) html += "<i>";
  html += "deep text";
  for (int i = 0; i < 35; i++) html += "</i>";
  const auto& spans = renderer_.render(html.c_str(), static_cast<int>(html.size()));
  ASSERT_EQ(spans.size(), 1u) << "expected 1 span";
  ASSERT_NE(spans[0].text, nullptr) << "span text is null";
  EXPECT_STREQ(spans[0].text, "deep text") << "text content";
  EXPECT_TRUE(spans[0].italic) << "italic style applied";
}

// ---------------------------------------------------------------------------
// B5: control characters in plain text — \n triggers line break, \t → space
// Reproduces F-060/F-061: pronunciation\n<p>definition</p> must not emit ◆
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, ControlChars_NewlineAndTab) {
  // \n between plain text and first <p> must produce a newline break, not a glyph
  const char* html = "pronunciation\n<p>definition</p>";
  const auto& spans = renderer_.render(html, static_cast<int>(strlen(html)));
  // Expected: span[0]="pronunciation" newlineBefore=false, span[1]="definition" newlineBefore=true
  ASSERT_EQ(spans.size(), 2u) << "expected 2 spans";
  ASSERT_NE(spans[0].text, nullptr);
  EXPECT_STREQ(spans[0].text, "pronunciation") << "first span text";
  EXPECT_FALSE(spans[0].newlineBefore) << "first span: no newlineBefore";
  ASSERT_NE(spans[1].text, nullptr);
  EXPECT_STREQ(spans[1].text, "definition") << "second span text";
  EXPECT_TRUE(spans[1].newlineBefore) << "second span: newlineBefore";
}

// ---------------------------------------------------------------------------
// Group C: isDictCodepoint unit tests
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, IsIpaCodepoint) {
  struct IpaCase {
    uint32_t cp;
    bool expected;
    const char* label;
  };
  const IpaCase cases[] = {
      {0x021F, false, "U+021F (below Latin Extended-B)"},
      {0x0220, true, "U+0220 (Latin Extended-B start)"},
      {0x02FF, true, "U+02FF (Modifier Letters end)"},
      {0x0300, false, "U+0300 (above Modifier Letters)"},
      {0x0370, true, "U+0370 (Greek start)"},
      {0x03FF, true, "U+03FF (Greek end)"},
      {0x0400, false, "U+0400 (Cyrillic)"},
      {0x1D00, true, "U+1D00 (Phonetic Extensions start)"},
      {0x1DBF, true, "U+1DBF (Phonetic Extensions Supplement end)"},
      {0x1DC0, false, "U+1DC0 (above Phonetic Ext Supplement)"},
      {0x0061, false, "U+0061 (ASCII 'a')"},
      {0x00E6, false, "U+00E6 (ae)"},
      {0x00F0, false, "U+00F0 (eth)"},
      {0x0153, false, "U+0153 (oe)"},
      {0x03B2, true, "U+03B2 (beta)"},
      {0x03B8, true, "U+03B8 (theta)"},
      {0x00E5, false, "U+00E5 (a-ring — not IPA)"},
      {0x03B1, true, "U+03B1 (alpha — Greek)"},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(isDictCodepoint(c.cp), c.expected) << c.label;
  }
}

// ---------------------------------------------------------------------------
// Group C: splitDictRuns unit tests
// ---------------------------------------------------------------------------
TEST_F(DictHtmlRendererTest, SplitIpaRuns_EmptyString) {
  std::vector<DictTextSpan> runs;
  splitDictRuns("", runs);
  EXPECT_TRUE(runs.empty()) << "empty string → 0 runs";
}

TEST_F(DictHtmlRendererTest, SplitIpaRuns_PureAscii) {
  std::vector<DictTextSpan> runs;
  splitDictRuns("abc", runs);
  ASSERT_EQ(runs.size(), 1u) << "pure ASCII → 1 run";
  EXPECT_FALSE(runs[0].isDictFont) << "not IPA";
  EXPECT_EQ(runs[0].text, "abc") << "text content";
}

TEST_F(DictHtmlRendererTest, SplitIpaRuns_SingleIpaCodepoint) {
  // U+0250 → UTF-8: 0xC9 0x90
  std::string ipa;
  ipa += '\xC9';
  ipa += '\x90';
  std::vector<DictTextSpan> runs;
  splitDictRuns(ipa.c_str(), runs);
  ASSERT_EQ(runs.size(), 1u) << "single IPA codepoint → 1 run";
  EXPECT_TRUE(runs[0].isDictFont) << "is IPA";
  EXPECT_EQ(runs[0].text, ipa) << "text content";
}

TEST_F(DictHtmlRendererTest, SplitIpaRuns_Mixed_AsciiIpaAscii) {
  // "abc" + U+0250 + "xyz" → 3 runs
  std::string mixed = "abc";
  mixed += '\xC9';
  mixed += '\x90';
  mixed += "xyz";
  std::vector<DictTextSpan> runs;
  splitDictRuns(mixed.c_str(), runs);
  ASSERT_EQ(runs.size(), 3u) << "mixed → 3 runs";
  EXPECT_FALSE(runs[0].isDictFont) << "run 0: not IPA";
  EXPECT_EQ(runs[0].text, "abc") << "run 0 text";
  EXPECT_TRUE(runs[1].isDictFont) << "run 1: IPA";
  EXPECT_FALSE(runs[2].isDictFont) << "run 2: not IPA";
  EXPECT_EQ(runs[2].text, "xyz") << "run 2 text";
}

TEST_F(DictHtmlRendererTest, SplitIpaRuns_ConsecutiveIpa) {
  // "ab" + U+0250 + U+0251 + "cd" → 3 runs, IPA run has 4 bytes
  std::string s = "ab";
  s += '\xC9';
  s += '\x90';  // U+0250
  s += '\xC9';
  s += '\x91';  // U+0251
  s += "cd";
  std::vector<DictTextSpan> runs;
  splitDictRuns(s.c_str(), runs);
  ASSERT_EQ(runs.size(), 3u) << "consecutive IPA → 3 runs";
  EXPECT_FALSE(runs[0].isDictFont);
  EXPECT_EQ(runs[0].text, "ab");
  EXPECT_TRUE(runs[1].isDictFont);
  EXPECT_EQ(runs[1].text.size(), 4u) << "IPA run: 4 bytes (two 2-byte codepoints)";
  EXPECT_FALSE(runs[2].isDictFont);
  EXPECT_EQ(runs[2].text, "cd");
}

TEST_F(DictHtmlRendererTest, SplitIpaRuns_CombiningMarkAttachesToIpaRun) {
  // U+0250 (IPA) + U+0301 (combining acute) → one IPA run of 4 bytes.
  std::string s;
  s += '\xC9';
  s += '\x90';  // U+0250
  s += '\xCC';
  s += '\x81';  // U+0301 combining acute
  std::vector<DictTextSpan> runs;
  splitDictRuns(s.c_str(), runs);
  ASSERT_EQ(runs.size(), 1u) << "IPA + combining mark → 1 run";
  EXPECT_TRUE(runs[0].isDictFont) << "is IPA";
  EXPECT_EQ(runs[0].text.size(), 4u) << "4 bytes total";
}
