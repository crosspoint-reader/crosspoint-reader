// Smoke test for DictHtmlRenderer.
// Reads all 6 entries from test/dictionaries/expat-full/ and passes each
// full entry to renderer.render() — exactly as on-device code would.
// Checks the complete span output against expected values.
//
// Build and run via test/run_dict_html_renderer_test.sh.
// Exit code: 0 = all pass, 1 = any failure or unexpected unknown tags.
// DICT_HTML_RENDERER_TRACK_UNKNOWN is defined by the build script via -D flag.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/DictHtmlRenderer/DictHtmlRenderer.h"

// ---------------------------------------------------------------------------
// Helper: read binary file
// ---------------------------------------------------------------------------
static std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "ERROR: cannot open " << path << "\n";
    return {};
  }
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

static void printSpan(int idx, const StyledSpan& s) {
  printf("  [%2d] \"%s\"", idx, s.text ? s.text : "(null)");
  if (s.bold) printf(" bold");
  if (s.italic) printf(" italic");
  if (s.underline) printf(" underline");
  if (s.strikethrough) printf(" strike");
  if (s.isListItem) printf(" listItem");
  if (s.newlineBefore) printf(" newline");
  if (s.indentLevel) printf(" indent=%d", s.indentLevel);
  printf("\n");
}

static void printExpected(int idx, const ExpectedSpan& e) {
  printf("  [%2d] \"%s\"", idx, e.text);
  if (e.bold) printf(" bold");
  if (e.italic) printf(" italic");
  if (e.underline) printf(" underline");
  if (e.strikethrough) printf(" strike");
  if (e.isListItem) printf(" listItem");
  if (e.newlineBefore) printf(" newline");
  if (e.indentLevel) printf(" indent=%d", e.indentLevel);
  printf("\n");
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

// Run one test entry. Passes full raw entry content to renderer.render(),
// the same way on-device lookup code does. Returns true on pass.
static bool runTest(const std::string& word, const std::string& content, DictHtmlRenderer& renderer,
                    const std::vector<ExpectedSpan>& expected, bool expectUnknownTags = false) {
  printf("\n=== %s ===\n", word.c_str());

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  renderer.currentEntryName = word.c_str();
  renderer.unknownTagCount = 0;
#endif

  const auto& spans = renderer.render(content.c_str(), static_cast<int>(content.size()));

  printf("  Spans (%zu):\n", spans.size());
  for (int i = 0; i < static_cast<int>(spans.size()); i++) {
    printSpan(i, spans[i]);
  }

  bool pass = true;

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  if (renderer.unknownTagCount > 0) {
    if (!expectUnknownTags) pass = false;
    for (int i = 0; i < renderer.unknownTagCount; i++) {
      const auto& u = renderer.unknownTags[i];
      printf("\n%s: Unknown tag encountered\n", expectUnknownTags ? "INFO" : "ERROR");
      printf("  Tag:     <%s>\n", u.tag);
      printf("  Entry:   %s\n", u.entry);
      printf("  Context: [%s] <%s>%s</%s> [%s]\n", u.wordBefore, u.tag, u.tagContents, u.tag, u.wordAfter);
      if (!expectUnknownTags) {
        printf("  Action:  Tag was blindly stripped. Add it to the renderer's tag registry\n");
        printf("           with an explicit handling decision (strip-keep, strip-all, format, etc.)\n");
        printf("           then add a test case for it to test/dictionaries/expat-full/.\n");
      }
    }
  }
#endif

  if (spans.size() != expected.size()) {
    printf("  FAIL: expected %zu spans, got %zu\n", expected.size(), spans.size());
    printf("  Expected:\n");
    for (int i = 0; i < static_cast<int>(expected.size()); i++) printExpected(i, expected[i]);
    return false;
  }

  for (int i = 0; i < static_cast<int>(expected.size()); i++) {
    if (!spanMatches(spans[i], expected[i])) {
      pass = false;
      printf("  FAIL at span [%d]:\n", i);
      printf("    got:      ");
      printSpan(i, spans[i]);
      printf("    expected: ");
      printExpected(i, expected[i]);
    }
  }

  if (pass) printf("  PASS\n");
  return pass;
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

// AbbrExpand
// Full entry:
//   <p>Three abbreviations should expand inline with their full title in parentheses.</p>
//   <p>Case 1: single-word title. Expected: c. (circa)</p>
//   <p>Case 2: multi-word title containing a space. Expected: AD (anno Domini)</p>
//   <p>Case 3: italic element inside abbr. Expected: f. (filius) with f. rendered italic.</p>
//   <p>----------</p>
//   <p>Abbreviation one: <abbr title="circa">c.</abbr></p>
//   <p>Abbreviation two: <abbr title="anno Domini">AD</abbr></p>
//   <p>Abbreviation three: <abbr title="filius"><i>f.</i></abbr></p>
//   <p>----------</p>
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

// BlockStrip
// Full entry:
//   <p>Nine block tags and all their children should be stripped entirely.</p>
//   <p>Nothing should appear between the two rules below.</p>
//   <p>Tags tested: hiero (...), svg (...), math (...), gallery, nowiki, poem, ref, REF, img (standalone).</p>
//   <p>----------</p>
//   [all block-strip tags — produce no spans]
//   <p>----------</p>
static const std::vector<ExpectedSpan> kBlockStrip = {
    S("Nine block tags and all their children should be stripped entirely.", true),
    S("Nothing should appear between the two rules below.", true),
    S("Tags tested: hiero (with nested table/tr/td/img), svg (with defs/g/path/rect), math (with sup), gallery, "
      "nowiki, poem, ref (lowercase), REF (uppercase), img (standalone).",
      true),
    S("----------", true),
    S("----------", true),
};

// BlockStruct
// Full entry:
//   <p>Block structure elements. Expected output in order:</p>
//   <p>Two separate paragraphs. Two div blocks. Three lines separated by br. ...</p>
//   <p>----------</p>
//   [block structure test HTML — 18 spans]
//   <p>----------</p>
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

// FormatTags
// Full entry:
//   <p>Inline formatting tags. Expected words with their styles:</p>
//   <p>bold (b), bold (strong), italic (i), italic (em), underline (u), ...</p>
//   <p>Nested: bold-italic (b+i). Triple: bold-italic-underline (b+i+u).</p>
//   <p>----------</p>
//   [format test HTML — 23 spans; FORMAT_SMALL tags merge with adjacent text]
//   <p>----------</p>
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

// StripKeep
// Full entry:
//   <p>Three cases where the tag is stripped but its text content is kept.</p>
//   <p>Case 1: span tag stripped, text kept. Expected: visible span text</p>
//   <p>Case 2: single unknown tag stripped, text kept. Expected: visible unknown text</p>
//   <p>Case 3: nested unknown tags both stripped, innermost text kept. ...</p>
//   <p>----------</p>
//   <p><span>visible span text</span></p>
//   <p><unknowntag>visible unknown text</unknowntag></p>
//   <p><outertag><innertag>visible nested text</innertag></outertag></p>
//   <p>----------</p>
// unknowntag / outertag / innertag are intentional unknown tags — expectUnknownTags=true.
static const std::vector<ExpectedSpan> kStripKeep = {
    S("Three cases where the tag is stripped but its text content is kept.", true),
    S("Case 1: span tag stripped, text kept. Expected: visible span text", true),
    S("Case 2: single unknown tag stripped, text kept. Expected: visible unknown text", true),
    S("Case 3: nested unknown tags both stripped, innermost text kept. Expected: visible nested text", true),
    S("----------", true),
    S("visible span text", true),
    S("visible unknown text", true),
    S("visible nested text", true),
    S("----------", true),
};

// WikiAnnot
// Full entry:
//   <p>Eight wikitext annotation tags. Each is a self-closing tag of the form XX:YY ...</p>
//   <p>Expected inline text in order: four, oikos, la, sharp, noun, Grek, ameba, female</p>
//   <p>----------</p>
//   <p>count: <t:four/> origin: <tr:oikos/> ... identifier: <id:female/></p>
//   <p>----------</p>
static const std::vector<ExpectedSpan> kWikiAnnot = {
    S("Eight wikitext annotation tags. Each is a self-closing tag of the form XX:YY where the text to render is the "
      "suffix YY (the part after the colon).",
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
    S("----------", true),
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  const char* dictDir = (argc > 1) ? argv[1] : "test/dictionaries/expat-full";

  std::string idxPath = std::string(dictDir) + "/expat-full.idx";
  std::string dictPath = std::string(dictDir) + "/expat-full.dict";

  std::string idxData = readFile(idxPath);
  std::string dictData = readFile(dictPath);

  if (idxData.empty() || dictData.empty()) {
    fprintf(stderr, "ERROR: Could not read dictionary files from %s\n", dictDir);
    return 1;
  }

  auto entries = parseIdx(idxData);
  if (entries.empty()) {
    fprintf(stderr, "ERROR: No entries found in idx\n");
    return 1;
  }

  printf("Loaded %zu entries from %s\n", entries.size(), dictDir);

  DictHtmlRenderer renderer;
  int passed = 0;
  int failed = 0;
  int unknownTagErrors = 0;

  struct TestCase {
    const char* word;
    const std::vector<ExpectedSpan>& expected;
    bool expectUnknownTags;
  };

  const TestCase tests[] = {
      {"BlazeSilent", kAbbrExpand, false},  {"ClearSvg", kBlockStrip, false},  {"DarkMath", kBlockStruct, false},
      {"EmptyGallery", kFormatTags, false}, {"FrostNowiki", kStripKeep, true}, {"GlowPoem", kWikiAnnot, false},
  };

  for (const auto& test : tests) {
    const DictEntry* found = nullptr;
    for (const auto& e : entries) {
      if (e.word == test.word) {
        found = &e;
        break;
      }
    }
    if (!found) {
      printf("\n=== %s ===\n  FAIL: entry not found in dictionary\n", test.word);
      failed++;
      continue;
    }
    if (found->offset + found->length > dictData.size()) {
      printf("\n=== %s ===\n  FAIL: entry out of bounds\n", test.word);
      failed++;
      continue;
    }

    std::string raw = dictData.substr(found->offset, found->length);

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
    renderer.unknownTagCount = 0;
    renderer.currentEntryName = test.word;
#endif

    bool pass = runTest(test.word, raw, renderer, test.expected, test.expectUnknownTags);

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
    if (renderer.unknownTagCount > 0 && !test.expectUnknownTags) unknownTagErrors += renderer.unknownTagCount;
    if (renderer.unknownTagCount > 0 && test.expectUnknownTags)
      printf("  (unknown tags expected for this entry — %d recorded)\n", renderer.unknownTagCount);
#endif

    if (pass)
      passed++;
    else
      failed++;
  }

  printf("\n--- Results: %d passed, %d failed", passed, failed);
  if (unknownTagErrors > 0) printf(", %d unexpected unknown tag(s)", unknownTagErrors);
  printf(" ---\n");

  return (failed > 0 || unknownTagErrors > 0) ? 1 : 0;
}
