// The CJK line-breaking rules (spec section 9.1), checked on every line the host pipeline lays out
// for the CJK corpus books: kinsoku at the edges of the lines the layout broke, no hyphen in CJK
// text, no space between two characters of unspaced CJK text (Han, kana, CJK punctuation), and every CJK
// mark at the width its book's punctuation table gives it (spec section 5.2).
//
// The books run left-aligned, so every gap on a line is its natural gap: zero at a CJK gap or a
// glued seam, the space advance at a word gap. Each book's paragraphs open with an indented first
// line (CSS text-indent), which tells a paragraph's first line from a line the layout broke.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Epub/CjkPunctuation.h"
#include "Epub/TypographyProfile.h"
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
  std::string shape;            // x, alignment and size of the LINE record: lines of one shape share a text width
  bool justified = false;       // align=0
  bool cell = false;            // a table cell line (LN record)
  int y = 0;
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
      next.cell = line.rfind("LN ", 0) == 0;
      const size_t mult = line.find(" mult=");
      if (mult != std::string::npos) next.sizeMultiplier = std::stof(line.substr(mult + 6));
      std::istringstream record(line);
      std::string field;
      std::string x;
      std::string align;
      std::string size;
      while (record >> field) {
        if (field.rfind("x=", 0) == 0) x = field;
        if (field.rfind("align=", 0) == 0) align = field;
        if (field.rfind("mult=", 0) == 0) size = field;
        if (field.rfind("y=", 0) == 0) next.y = std::stoi(field.substr(2));
      }
      next.shape = x + " " + align + " " + size;
      next.justified = align == "align=0";
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

// The block each line belongs to: a body line's shape (x, alignment and size, so one text width and left edge), a
// table cell line's own cell.
std::vector<std::string> blockKeys(const std::vector<DumpLine>& lines) {
  std::vector<std::string> keys;
  size_t cell = 0;
  for (const auto& line : lines) {
    if (line.cell && !line.followsLine) ++cell;
    keys.push_back(line.cell ? "cell " + std::to_string(cell) : line.shape);
  }
  return keys;
}

// Where each block's leftmost line starts. A line that starts further right is a paragraph's indented first line
// (or a centred line); with the CJK grid every line of a block starts a few pixels in.
std::map<std::string, int> blockLeftEdges(const GfxRenderer& renderer, const std::vector<DumpLine>& lines,
                                          const std::vector<std::string>& keys,
                                          const TypographyProfile::Punctuation table) {
  std::map<std::string, int> edges;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].words.empty()) continue;
    const int left = LineWidths(renderer, lines[i], table).left(0);
    const auto [at, inserted] = edges.emplace(keys[i], left);
    if (!inserted) at->second = std::min(at->second, left);
  }
  return edges;
}

struct Case {
  std::string epub;
  std::string language;
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
                           (c.hyphenation ? "_hyph" : "") + (alignment() == 0 ? "_justified" : ""));
    fs::remove_all(cacheDir);
    fs::create_directories(cacheDir);
    pipeline_harness::Profile profile;
    profile.paragraphAlignment = alignment();
    profile.hyphenationEnabled = c.hyphenation;
    table = TypographyProfile::forLanguage(c.language).punctuation;
    std::ostringstream dump;
    ASSERT_TRUE(pipeline_harness::runAndDump(c.epub, cacheDir.string(), profile, dump)) << dump.str();
    lines = parseLines(dump.str());
    ASSERT_FALSE(lines.empty());
  }

  int wordWidth(const DumpLine& line, const DumpWord& w) const { return ::wordWidth(renderer, line, w); }
  // The reader's paragraph alignment: left (1), where a line's gaps are its natural gaps.
  virtual uint8_t alignment() const { return 1; }

  GfxRenderer renderer;
  TypographyProfile::Punctuation table = TypographyProfile::Punctuation::SimplifiedChinese;
  std::vector<DumpLine> lines;
};

// The same books laid out justified.
class CjkJustifiedRules : public CjkRules {
 protected:
  uint8_t alignment() const override { return 0; }
};

// At a line break with unspaced CJK on either side, the line after it does not start with closing
// punctuation and the line before it does not end with an opening bracket (spec Appendix A.1): a
// line starting with " after a CJK line, or ending with " before one, is caught too.
TEST_P(CjkRules, KinsokuHoldsAtEveryCjkLineBreak) {
  const std::vector<std::string> keys = blockKeys(lines);
  const std::map<std::string, int> leftEdge = blockLeftEdges(renderer, lines, keys, table);
  size_t breaks = 0;   // line breaks with unspaced CJK on either side
  size_t checked = 0;  // those that are layout breaks, not a paragraph's start
  for (size_t i = 1; i < lines.size(); ++i) {
    const DumpLine& prev = lines[i - 1];
    const DumpLine& line = lines[i];
    if (!line.followsLine || prev.words.empty() || line.words.empty()) continue;
    const uint32_t end = lastCodepoint(prev.words.back().text);
    const uint32_t start = firstCodepoint(line.words.front().text);
    if (!utf8IsUnspacedCjk(end) && !utf8IsUnspacedCjk(start)) continue;
    ++breaks;
    // A paragraph's indented first line is not a layout break.
    if (LineWidths(renderer, line, table).left(0) > leftEdge.at(keys[i])) continue;
    ++checked;
    EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end))
        << "line ends with an opening bracket: " << lineText(prev) << " before: " << lineText(line);
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(start))
        << "line starts with closing punctuation: " << lineText(line) << " after: " << lineText(prev);
  }
  // Paragraph starts are the fewer of a book's line breaks: a check that skips most of them checks nothing.
  EXPECT_GE(checked * 2, breaks) << "only " << checked << " of " << breaks << " CJK line breaks checked";
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

// A justified line with CJK gaps that is not its paragraph's last ends flush (spec 5.1), all at one right edge,
// and in a Chinese or Japanese book the text width is a whole number of ems (spec 5.5). Lines with one LINE record
// shape (x, alignment, size) share a text width; the shape's left edge is where its leftmost line starts. A line
// is not its paragraph's last when the next line of its flow has the same shape, starts at that left edge (not
// indented) and follows without a margin. Lines justified at word spaces (Latin, Korean) keep their old rounding
// and are not checked.
TEST_P(CjkJustifiedRules, JustifiedLinesEndFlushOnAWholeEmGrid) {
  const TypographyProfile profile = TypographyProfile::forLanguage(GetParam().language);
  if (profile.justify != TypographyProfile::Justify::BetweenCharacters) GTEST_SKIP() << "justified at word spaces";

  const std::vector<std::string> keys = blockKeys(lines);
  std::map<std::string, int> leftEdge = blockLeftEdges(renderer, lines, keys, table);  // shape -> its left edge
  std::map<std::string, int> lineStep;  // shape -> its smallest step down the page
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].words.empty()) continue;
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

std::vector<Case> cjkBooks() {
  std::vector<Case> cases;
  for (const auto& [book, language] :
       std::vector<std::pair<const char*, const char*>>{{"test_cjk_line_breaking", "ja"},
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
INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkJustifiedRules, testing::ValuesIn(cjkBooks()), caseName);

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
