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
    const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" /
                          ("cjk_rules_" + fs::path(c.epub).stem().string() + (c.hyphenation ? "_hyph" : ""));
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

  // As ParsedText measures a word: the block's multiplier times the word's own size, rounded.
  int wordWidth(const DumpLine& line, const DumpWord& w) const {
    const int raw = renderer.getTextAdvanceX(1, w.text.c_str(), static_cast<EpdFontFamily::Style>(w.style));
    const float scale = line.sizeMultiplier * (w.sizePct / 100.0f);
    return scale == 1.0f ? raw : static_cast<int>(raw * scale + 0.5f);
  }

  GfxRenderer renderer;
  std::vector<DumpLine> lines;
};

// A line the layout broke between two unspaced CJK characters does not start with closing
// punctuation and the line before it does not end with an opening bracket (spec Appendix A.1).
TEST_P(CjkRules, KinsokuHoldsAtEveryCjkLineBreak) {
  for (size_t i = 1; i < lines.size(); ++i) {
    const DumpLine& prev = lines[i - 1];
    const DumpLine& line = lines[i];
    if (!line.followsLine || prev.words.empty() || line.words.empty()) continue;
    if (line.words.front().x > 0) continue;  // a paragraph's indented first line: not a layout break
    const uint32_t end = lastCodepoint(prev.words.back().text);
    const uint32_t start = firstCodepoint(line.words.front().text);
    if (!utf8IsUnspacedCjk(end) || !utf8IsUnspacedCjk(start)) continue;
    EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end)) << "line ends with an opening bracket: " << lineText(prev);
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(start)) << "line starts with closing punctuation: " << lineText(line);
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
// case 8, a cell of 385 CJK characters (18 KB of tokens), passes the 12 KB row budget and flows as
// text, as a cell of 385 Latin words does. Charged once per flushed run, it would count as a dozen
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
