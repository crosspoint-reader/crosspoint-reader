// Host-side (Tier-A) litmus for DictLayout — the pure wrap/pagination module.
//
// Verifies the logic the Stage 2a streaming page-collector depends on, with a
// deterministic fake measurer (no fonts, no SD, no expat): golden wrap output,
// paginate(), and that the streaming sink + one-page windowing reproduces the
// reference full-layout oracle exactly. Runs on the dev host; OOM is not
// observable here (abundant RAM) — this guards correctness, the device guards
// memory.
//

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "util/DictLayout.h"

// --------------------------------------------------------------------------
// Fixtures
// --------------------------------------------------------------------------

// Deterministic measurer: 1px per UTF-8 byte, independent of style/IPA. Makes
// wrap decisions hand-computable.
static int fakeMeasure(void* /*ctx*/, const char* text, EpdFontFamily::Style /*style*/, bool /*isDictFont*/) {
  return static_cast<int>(std::strlen(text));
}

static StyledSpan mkSpan(const char* text, bool newlineBefore = false, bool bold = false, bool italic = false,
                         uint8_t indentLevel = 0, bool isListItem = false) {
  StyledSpan s;
  s.text = text;
  s.newlineBefore = newlineBefore;
  s.bold = bold;
  s.italic = italic;
  s.indentLevel = indentLevel;
  s.isListItem = isListItem;
  return s;
}

// Concatenate a line's segment texts (for readable assertions).
static std::string lineText(const DictLayout::LayoutLine& line) {
  std::string out;
  for (const auto& seg : line.segments) out += seg.text;
  return out;
}

static bool linesEqual(const DictLayout::LayoutLine& a, const DictLayout::LayoutLine& b) {
  if (a.indentLevel != b.indentLevel || a.isListItem != b.isListItem) return false;
  if (a.segments.size() != b.segments.size()) return false;
  for (size_t i = 0; i < a.segments.size(); ++i) {
    if (a.segments[i].text != b.segments[i].text) return false;
    if (a.segments[i].style != b.segments[i].style) return false;
    if (a.segments[i].isDictFont != b.segments[i].isDictFont) return false;
  }
  return true;
}

// Mirrors DictionaryDefinitionActivity::collectLineSink — keep only the target
// page's lines, count all. Kept in-test so the litmus exercises the windowing
// math against the oracle without pulling in the activity.
struct PageCollector {
  int targetPage = 0;
  int linesPerPage = 1;
  int count = 0;
  std::vector<DictLayout::LayoutLine> kept;

  static void onLine(void* ctx, DictLayout::LayoutLine&& line) {
    auto* self = static_cast<PageCollector*>(ctx);
    const int idx = self->count++;
    const int start = self->targetPage * self->linesPerPage;
    if (idx >= start && idx < start + self->linesPerPage) self->kept.push_back(std::move(line));
  }

  DictLayout::LineSink sink() { return DictLayout::LineSink{this, &PageCollector::onLine}; }
};

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

TEST(DictLayout, SpanFitsOneLine) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("hello")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 1u) << "one line";
  EXPECT_EQ(out[0].segments.size(), 1u) << "one segment";
  EXPECT_EQ(lineText(out[0]), "hello") << "text preserved";
}

TEST(DictLayout, WordWrap) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // "aaa bbb ccc": each word 3px, space 1px. maxWidth 7 → "aaa bbb" fills (7), "ccc" wraps.
  std::vector<StyledSpan> spans{mkSpan("aaa bbb ccc")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{7, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 2u) << "wraps to two lines";
  EXPECT_EQ(lineText(out[0]), "aaa bbb") << "first line packs two words";
  EXPECT_EQ(lineText(out[1]), "ccc") << "third word on second line";
}

TEST(DictLayout, NewlineBeforeStartsLine) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("aaa"), mkSpan("bbb", /*newlineBefore=*/true)};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 2u) << "newlineBefore forces a new line";
  EXPECT_EQ(lineText(out[0]), "aaa") << "line 0";
  EXPECT_EQ(lineText(out[1]), "bbb") << "line 1";
}

TEST(DictLayout, StyleAndListIndent) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("bold", /*nl=*/true, /*bold=*/true, /*italic=*/false, /*indent=*/2,
                                       /*list=*/true)};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 1u) << "one line";
  EXPECT_EQ(out[0].indentLevel, 2) << "indent propagated";
  EXPECT_TRUE(out[0].isListItem) << "list flag propagated";
  ASSERT_FALSE(out[0].segments.empty()) << "has segments";
  EXPECT_EQ(out[0].segments[0].style, EpdFontFamily::BOLD) << "bold style";
}

TEST(DictLayout, AdjacentSameStyleMerges) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // Two REGULAR spans on the same line should coalesce into one segment.
  std::vector<StyledSpan> spans{mkSpan("foo"), mkSpan("bar")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 1u) << "one line";
  EXPECT_EQ(out[0].segments.size(), 1u) << "same-style segments merge";
}

TEST(DictLayout, Paginate) {
  EXPECT_EQ(DictLayout::paginate(0, 2), 1) << "zero lines -> 1 page (clamped)";
  EXPECT_EQ(DictLayout::paginate(1, 2), 1) << "1/2 -> 1";
  EXPECT_EQ(DictLayout::paginate(2, 2), 1) << "2/2 -> 1";
  EXPECT_EQ(DictLayout::paginate(3, 2), 2) << "3/2 -> 2";
  EXPECT_EQ(DictLayout::paginate(4, 2), 2) << "4/2 -> 2";
  EXPECT_EQ(DictLayout::paginate(5, 2), 3) << "5/2 -> 3";
  EXPECT_EQ(DictLayout::paginate(10, 0), 10) << "linesPerPage<1 clamps to 1";
}

// The gold invariant: streaming through the page-collector for page P yields
// exactly the same lines as the reference full layout sliced to page P, and the
// total line count matches. This is what makes "re-parse to page N" correct.
TEST(DictLayout, PageCollectorMatchesOracle) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // Force many short lines via newlineBefore so we get a known multi-page layout.
  std::vector<StyledSpan> spans;
  static const char* words[] = {"a", "b", "c", "d", "e", "f", "g"};
  for (int i = 0; i < 7; ++i) spans.push_back(mkSpan(words[i], /*newlineBefore=*/i != 0));

  std::vector<DictLayout::LayoutLine> oracle;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, oracle);
  ASSERT_EQ(oracle.size(), 7u) << "oracle has 7 lines";

  const int linesPerPage = 3;  // pages: [a b c] [d e f] [g]
  const int pages = DictLayout::paginate(static_cast<int>(oracle.size()), linesPerPage);
  ASSERT_EQ(pages, 3) << "3 pages";

  for (int p = 0; p < pages; ++p) {
    SCOPED_TRACE("page " + std::to_string(p));
    PageCollector pc;
    pc.targetPage = p;
    pc.linesPerPage = linesPerPage;
    DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, pc.sink());

    EXPECT_EQ(pc.count, static_cast<int>(oracle.size())) << "collector counts all lines";

    const int start = p * linesPerPage;
    const int expectedKept = std::min(linesPerPage, static_cast<int>(oracle.size()) - start);
    EXPECT_EQ(static_cast<int>(pc.kept.size()), expectedKept) << "collector keeps exactly one page";
    for (int i = 0; i < expectedKept; ++i) {
      EXPECT_TRUE(linesEqual(pc.kept[i], oracle[start + i])) << "kept line " << i << " matches oracle slice";
    }
  }
}

TEST(DictLayout, TrailingSpacePreservedOnWordWrap) {
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // Span 1: "xx" (2 chars)
  // Span 2: "in the " (7 chars, including trailing space).
  // Span 3: "shaw" (4 chars, bold).
  // With maxWidth = 8, "in the " will exceed maxWidth and go to the word-wrap path.
  // It should wrap to:
  // Line 0: "xx in"
  // Line 1: "the " + "shaw" -> "the shaw"
  std::vector<StyledSpan> spans{mkSpan("xx "), mkSpan("in the "), mkSpan("shaw", false, /*bold=*/true)};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{8, 0, 0}, meas, out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(lineText(out[0]), "xx in");
  EXPECT_EQ(lineText(out[1]), "the shaw");
  ASSERT_EQ(out[1].segments.size(), 2u);
  EXPECT_EQ(out[1].segments[0].text, "the ");
  EXPECT_EQ(out[1].segments[1].text, "shaw");
}
