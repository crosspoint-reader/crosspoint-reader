// The CJK line-breaking rules (spec section 9.1), checked on every line the host pipeline lays out
// for the CJK corpus books: kinsoku at the edges of the lines the layout broke, no hyphen in CJK
// text, and no space between two characters of unspaced CJK text (Han, kana, CJK punctuation).
//
// The books run left-aligned, so every gap on a line is its natural gap: zero at a CJK gap or a
// glued seam, the space advance at a word gap. Each book's paragraphs open with an indented first
// line (CSS text-indent), which tells a paragraph's first line from a line the layout broke.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct DumpWord {
  int x = 0;
  int style = 0;
  int sizePct = 100;
  std::string text;
};

struct DumpLine {
  std::vector<DumpWord> words;
  float sizeMultiplier = 1.0f;  // the block's font-size multiplier (LINE mult=); table cell lines have none
  bool followsLine = false;     // the line before it in `lines` is the previous line of the same flow
};

uint32_t firstCodepoint(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&p);
}

uint32_t lastCodepoint(const std::string& text) {
  size_t i = text.size();
  while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str() + i - 1);
  return utf8NextCodepoint(&p);
}

// The text lines of a dump in reading order: body lines (LINE) and table cell lines (LN). A spine,
// table, row or cell starts a new flow; a page break does not.
std::vector<DumpLine> parseLines(const std::string& dump) {
  std::vector<DumpLine> lines;
  bool inFlow = false;
  std::istringstream in(dump);
  std::string raw;
  while (std::getline(in, raw)) {
    const size_t start = raw.find_first_not_of(' ');
    if (start == std::string::npos) continue;
    const std::string line = raw.substr(start);
    if (line.rfind("LINE ", 0) == 0 || line.rfind("LN ", 0) == 0) {
      DumpLine next;
      next.followsLine = inFlow;
      const size_t mult = line.find(" mult=");
      if (mult != std::string::npos) next.sizeMultiplier = std::stof(line.substr(mult + 6));
      lines.push_back(next);
      inFlow = true;
    } else if (line.rfind("W x=", 0) == 0 && !lines.empty()) {
      DumpWord word;
      const size_t textAt = line.find(" t=");
      std::istringstream fields(line.substr(0, textAt));
      std::string field;
      while (fields >> field) {
        if (field.rfind("x=", 0) == 0) word.x = std::stoi(field.substr(2));
        if (field.rfind("s=", 0) == 0) word.style = std::stoi(field.substr(2));
        if (field.rfind("z=", 0) == 0) word.sizePct = std::stoi(field.substr(2));
      }
      word.text = textAt == std::string::npos ? "" : line.substr(textAt + 3);
      lines.back().words.push_back(word);
    } else if (line.rfind("SPINE ", 0) == 0 || line.rfind("TABLE ", 0) == 0 || line.rfind("ROW ", 0) == 0 ||
               line.rfind("CELL ", 0) == 0) {
      inFlow = false;
    }
  }
  return lines;
}

std::string lineText(const DumpLine& line) {
  std::string text;
  for (const auto& w : line.words) text += w.text + "|";
  return text;
}

// As ParsedText measures a word: the block's multiplier times the word's own size, rounded.
int wordWidth(const GfxRenderer& renderer, const DumpLine& line, const DumpWord& w) {
  const int raw = renderer.getTextAdvanceX(1, w.text.c_str(), static_cast<EpdFontFamily::Style>(w.style));
  const float scale = line.sizeMultiplier * (w.sizePct / 100.0f);
  return scale == 1.0f ? raw : static_cast<int>(raw * scale + 0.5f);
}

struct Case {
  std::string epub;
  bool hyphenation = false;
};

void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << (c.hyphenation ? " [hyphenation]" : "");
}

class CjkRules : public testing::TestWithParam<Case> {
 protected:
  void SetUp() override {
    const Case c = GetParam();
    // One directory per test, not per book: ctest -j runs a book's three tests side by side, and one
    // test's remove_all would pull the cache from under another's build.
    const std::string test = testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" /
                          ("cjk_rules_" + test.substr(0, test.find('/')) + "_" + fs::path(c.epub).stem().string() +
                           (c.hyphenation ? "_hyph" : ""));
    fs::remove_all(cacheDir);
    fs::create_directories(cacheDir);
    pipeline_harness::Profile profile;
    profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
    profile.hyphenationEnabled = c.hyphenation;
    std::ostringstream dump;
    ASSERT_TRUE(pipeline_harness::runAndDump(c.epub, cacheDir.string(), profile, dump)) << dump.str();
    lines = parseLines(dump.str());
    ASSERT_FALSE(lines.empty());
  }

  int wordWidth(const DumpLine& line, const DumpWord& w) const { return ::wordWidth(renderer, line, w); }

  GfxRenderer renderer;
  std::vector<DumpLine> lines;
};

// At a line break with unspaced CJK on either side, the line after it does not start with closing
// punctuation and the line before it does not end with an opening bracket (spec Appendix A.1): a
// line starting with " after a CJK line, or ending with " before one, is caught too.
TEST_P(CjkRules, KinsokuHoldsAtEveryCjkLineBreak) {
  for (size_t i = 1; i < lines.size(); ++i) {
    const DumpLine& prev = lines[i - 1];
    const DumpLine& line = lines[i];
    if (!line.followsLine || prev.words.empty() || line.words.empty()) continue;
    if (line.words.front().x > 0) continue;  // a paragraph's indented first line: not a layout break
    const uint32_t end = lastCodepoint(prev.words.back().text);
    const uint32_t start = firstCodepoint(line.words.front().text);
    if (!utf8IsUnspacedCjk(end) && !utf8IsUnspacedCjk(start)) continue;
    EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end))
        << "line ends with an opening bracket: " << lineText(prev) << " before: " << lineText(line);
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(start))
        << "line starts with closing punctuation: " << lineText(line) << " after: " << lineText(prev);
  }
}

// CJK scripts are not hyphenated: no line ends with a hyphen after a CJK character.
TEST_P(CjkRules, NoHyphenAfterCjkText) {
  for (const auto& line : lines) {
    if (line.words.empty()) continue;
    const std::string& last = line.words.back().text;
    if (last.size() < 2 || last.back() != '-') continue;
    EXPECT_FALSE(utf8IsCjkBreakable(lastCodepoint(last.substr(0, last.size() - 1))))
        << "hyphen after CJK text: " << lineText(line);
  }
}

// Between two characters of unspaced CJK text there is no space: not where the source broke a line,
// not where the parser flushed a long run, not across an inline element.
TEST_P(CjkRules, NoSpaceBetweenUnspacedCjkCharacters) {
  for (const auto& line : lines) {
    for (size_t i = 0; i + 1 < line.words.size(); ++i) {
      const DumpWord& left = line.words[i];
      const DumpWord& right = line.words[i + 1];
      if (!utf8IsUnspacedCjk(lastCodepoint(left.text)) || !utf8IsUnspacedCjk(firstCodepoint(right.text))) continue;
      EXPECT_EQ(right.x - left.x - wordWidth(line, left), 0)
          << "gap between \"" << left.text << "\" and \"" << right.text << "\" in: " << lineText(line);
    }
  }
}

std::vector<Case> cjkBooks() {
  std::vector<Case> cases;
  for (const char* book :
       {"test_cjk_line_breaking", "test_cjk_ja_soseki", "test_cjk_ko_hyeon", "test_cjk_zh_honglou"}) {
    const std::string path = (fs::path(CORPUS_DIR) / (std::string(book) + ".epub")).string();
    cases.push_back(Case{path, false});
    cases.push_back(Case{path, true});
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkRules, testing::ValuesIn(cjkBooks()),
                         [](const testing::TestParamInfo<Case>& info) {
                           return fs::path(info.param.epub).stem().string() + (info.param.hyphenation ? "_hyph" : "");
                         });

}  // namespace

// A CJK word is one token per character, and the table row budget charges every token: the fixture's
// case 8, a cell of 289 CJK characters (14 KB of tokens), passes the 12 KB row budget and flows as
// text, as a cell of 289 Latin words does. Charged once per flushed run, it would count as a dozen
// words and stay buffered.
TEST(CjkTable, ALongCjkCellIsChargedPerToken) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_table_budget";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_line_breaking.epub").string(),
                                           cacheDir.string(), pipeline_harness::Profile{}, dump));
  std::istringstream in(dump.str());
  std::string raw;
  std::string lastLineRecord;
  while (std::getline(in, raw)) {
    const std::string line = raw.substr(raw.find_first_not_of(' '));
    if (line.rfind("LINE ", 0) == 0 || line.rfind("LN ", 0) == 0) lastLineRecord = line;
    if (line.find(" t=鑑") != std::string::npos) {
      EXPECT_EQ(lastLineRecord.rfind("LINE ", 0), 0u) << "case 8 stayed in a table cell: " << lastLineRecord;
      return;
    }
  }
  FAIL() << "case 8's first character is missing from the dump";
}

// A source line break next to an element that is not inline formatting is a word gap, not a CJK
// segment break to drop (test_cjk_block_seams.epub; not in cjkBooks(), since these spaces are
// correct). A list item whose text sits on its own indented line keeps the space after its marker
// that its one-line control has, and the text of <dl>, <section> and <figure>, which the parser lays
// out in one block, keeps a space at each seam.
TEST(CjkBlockSeams, ALineBreakAtABlockOrListItemStaysASpace) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_block_seams";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  pipeline_harness::Profile profile;
  profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_block_seams.epub").string(),
                                           cacheDir.string(), profile, dump));
  const std::vector<DumpLine> lines = parseLines(dump.str());
  const GfxRenderer renderer;

  // The gap between words[i] and words[i + 1] of the line where `left` is followed by `right`, or
  // -1 when no line has them side by side.
  const auto gapBetween = [&](const std::string& left, const std::string& right) {
    for (const auto& line : lines) {
      for (size_t i = 0; i + 1 < line.words.size(); ++i) {
        if (line.words[i].text == left && line.words[i + 1].text == right) {
          return line.words[i + 1].x - line.words[i].x - wordWidth(renderer, line, line.words[i]);
        }
      }
    }
    return -1;
  };
  // The gap between an item's marker and the first character of its text, 第 followed by `second`.
  const auto markerGap = [&](const std::string& second) {
    for (const auto& line : lines) {
      for (size_t i = 1; i + 1 < line.words.size(); ++i) {
        if (line.words[i].text == "第" && line.words[i + 1].text == second) {
          return line.words[i].x - line.words[i - 1].x - wordWidth(renderer, line, line.words[i - 1]);
        }
      }
    }
    return -1;
  };

  // Pretty-printed item and one-line control: the <ul> pair, then the <ol> pair. The <ol> markers
  // (1. and 2.) differ in width, so each pair compares the gap after the marker.
  ASSERT_GT(markerGap("二"), 0) << "the <ul> control item is missing";
  EXPECT_EQ(markerGap("一"), markerGap("二")) << "the pretty-printed <ul> item is glued to its marker";
  ASSERT_GT(markerGap("四"), 0) << "the <ol> control item is missing";
  EXPECT_EQ(markerGap("三"), markerGap("四")) << "the pretty-printed <ol> item is glued to its marker";

  // When the two sides share a line, a space separates them.
  if (const int gap = gapBetween("語", "説"); gap != -1) {
    EXPECT_GT(gap, 0) << "用語 and 説明文 glued";
  }
  if (const int gap = gapBetween("章", "図"); gap != -1) {
    EXPECT_GT(gap, 0) << "節の文章 and 図の説明 glued";
  }
}

// In CJK text the quotation marks are CJK punctuation: a source line break between one and unspaced
// CJK text vanishes, as one between two CJK characters does, so 樂業。\n”石頭 reads 樂業。”石頭 and ”
// cannot start a line (the fixture's case 10, after Hong lou meng's hard-wrapped source). With
// quotes on both sides, the line break stays a space.
TEST(CjkQuoteSeams, ALineBreakNextToAQuoteInCjkTextVanishes) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_quote_seams";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  pipeline_harness::Profile profile;
  profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_line_breaking.epub").string(),
                                           cacheDir.string(), profile, dump));
  const std::vector<DumpLine> lines = parseLines(dump.str());
  const GfxRenderer renderer;

  // The gap before every word starting with `right` whose left neighbour on its line ends with `left`.
  const auto gapsAt = [&](const uint32_t left, const uint32_t right) {
    std::vector<int> gaps;
    for (const auto& line : lines) {
      for (size_t i = 0; i + 1 < line.words.size(); ++i) {
        const DumpWord& l = line.words[i];
        const DumpWord& r = line.words[i + 1];
        if (lastCodepoint(l.text) == left && firstCodepoint(r.text) == right) {
          gaps.push_back(r.x - l.x - wordWidth(renderer, line, l));
        }
      }
    }
    return gaps;
  };

  const struct {
    uint32_t left;
    uint32_t right;
    const char* seam;
  } glued[] = {
      {0x3002, 0x201D, "。 + ”"}, {0xFF1A, '"', "： + \""}, {0x201D, 0x5144, "” + 兄"}, {0x5B57, 0x201C, "字 + “"}};
  for (const auto& s : glued) {
    const std::vector<int> gaps = gapsAt(s.left, s.right);
    ASSERT_FALSE(gaps.empty()) << "no line has the seam " << s.seam;
    for (const int gap : gaps) EXPECT_EQ(gap, 0) << "the line break at " << s.seam << " stayed a space";
  }
  const std::vector<int> control = gapsAt(0x201D, 0x201C);  // ” + “
  ASSERT_FALSE(control.empty()) << "no line has the seam ” + “";
  for (const int gap : control) EXPECT_GT(gap, 0) << "the line break between two quotes lost its space";
}
