// Host-side (Tier-A) litmus for DictLayout — the pure wrap/pagination module.
//
// Verifies the logic the Stage 2a streaming page-collector depends on, with a
// deterministic fake measurer (no fonts, no SD, no expat): golden wrap output,
// paginate(), and that the streaming sink + one-page windowing reproduces the
// reference full-layout oracle exactly. Runs on the dev host; OOM is not
// observable here (abundant RAM) — this guards correctness, the device guards
// memory.
//
// Build/run: test/dict-layout/run.sh

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util/DictLayout.h"

// --------------------------------------------------------------------------
// Tiny test harness
// --------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// --------------------------------------------------------------------------
// Fixtures
// --------------------------------------------------------------------------

// Deterministic measurer: 1px per UTF-8 byte, independent of style/IPA. Makes
// wrap decisions hand-computable.
static int fakeMeasure(void* /*ctx*/, const char* text, EpdFontFamily::Style /*style*/, bool /*isIpa*/) {
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
    if (a.segments[i].isIpa != b.segments[i].isIpa) return false;
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

static void testSpanFitsOneLine() {
  std::printf("testSpanFitsOneLine\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("hello")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  CHECK(out.size() == 1, "one line");
  if (out.size() == 1) {
    CHECK(out[0].segments.size() == 1, "one segment");
    CHECK(lineText(out[0]) == "hello", "text preserved");
  }
}

static void testWordWrap() {
  std::printf("testWordWrap\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // "aaa bbb ccc": each word 3px, space 1px. maxWidth 7 → "aaa bbb" fills (7), "ccc" wraps.
  std::vector<StyledSpan> spans{mkSpan("aaa bbb ccc")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{7, 0, 0}, meas, out);
  CHECK(out.size() == 2, "wraps to two lines");
  if (out.size() == 2) {
    CHECK(lineText(out[0]) == "aaa bbb", "first line packs two words");
    CHECK(lineText(out[1]) == "ccc", "third word on second line");
  }
}

static void testNewlineBeforeStartsLine() {
  std::printf("testNewlineBeforeStartsLine\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("aaa"), mkSpan("bbb", /*newlineBefore=*/true)};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  CHECK(out.size() == 2, "newlineBefore forces a new line");
  if (out.size() == 2) {
    CHECK(lineText(out[0]) == "aaa", "line 0");
    CHECK(lineText(out[1]) == "bbb", "line 1");
  }
}

static void testStyleAndListIndent() {
  std::printf("testStyleAndListIndent\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  std::vector<StyledSpan> spans{mkSpan("bold", /*nl=*/true, /*bold=*/true, /*italic=*/false, /*indent=*/2,
                                       /*list=*/true)};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  CHECK(out.size() == 1, "one line");
  if (out.size() == 1) {
    CHECK(out[0].indentLevel == 2, "indent propagated");
    CHECK(out[0].isListItem == true, "list flag propagated");
    CHECK(!out[0].segments.empty() && out[0].segments[0].style == EpdFontFamily::BOLD, "bold style");
  }
}

static void testAdjacentSameStyleMerges() {
  std::printf("testAdjacentSameStyleMerges\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // Two REGULAR spans on the same line should coalesce into one segment.
  std::vector<StyledSpan> spans{mkSpan("foo"), mkSpan("bar")};
  std::vector<DictLayout::LayoutLine> out;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, out);
  CHECK(out.size() == 1, "one line");
  if (out.size() == 1) CHECK(out[0].segments.size() == 1, "same-style segments merge");
}

static void testPaginate() {
  std::printf("testPaginate\n");
  CHECK(DictLayout::paginate(0, 2) == 1, "zero lines -> 1 page (clamped)");
  CHECK(DictLayout::paginate(1, 2) == 1, "1/2 -> 1");
  CHECK(DictLayout::paginate(2, 2) == 1, "2/2 -> 1");
  CHECK(DictLayout::paginate(3, 2) == 2, "3/2 -> 2");
  CHECK(DictLayout::paginate(4, 2) == 2, "4/2 -> 2");
  CHECK(DictLayout::paginate(5, 2) == 3, "5/2 -> 3");
  CHECK(DictLayout::paginate(10, 0) == 10, "linesPerPage<1 clamps to 1");
}

// The gold invariant: streaming through the page-collector for page P yields
// exactly the same lines as the reference full layout sliced to page P, and the
// total line count matches. This is what makes "re-parse to page N" correct.
static void testPageCollectorMatchesOracle() {
  std::printf("testPageCollectorMatchesOracle\n");
  const DictLayout::Measurer meas{nullptr, &fakeMeasure};
  // Force many short lines via newlineBefore so we get a known multi-page layout.
  std::vector<StyledSpan> spans;
  static const char* words[] = {"a", "b", "c", "d", "e", "f", "g"};
  for (int i = 0; i < 7; ++i) spans.push_back(mkSpan(words[i], /*newlineBefore=*/i != 0));

  std::vector<DictLayout::LayoutLine> oracle;
  DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, oracle);
  CHECK(oracle.size() == 7, "oracle has 7 lines");

  const int linesPerPage = 3;  // pages: [a b c] [d e f] [g]
  const int pages = DictLayout::paginate(static_cast<int>(oracle.size()), linesPerPage);
  CHECK(pages == 3, "3 pages");

  for (int p = 0; p < pages; ++p) {
    PageCollector pc;
    pc.targetPage = p;
    pc.linesPerPage = linesPerPage;
    DictLayout::wrapSpans(spans, DictLayout::WrapMetrics{100, 0, 0}, meas, pc.sink());

    CHECK(pc.count == static_cast<int>(oracle.size()), "collector counts all lines");

    const int start = p * linesPerPage;
    const int expectedKept = std::min(linesPerPage, static_cast<int>(oracle.size()) - start);
    CHECK(static_cast<int>(pc.kept.size()) == expectedKept, "collector keeps exactly one page");
    for (int i = 0; i < expectedKept; ++i) {
      CHECK(linesEqual(pc.kept[i], oracle[start + i]), "kept line matches oracle slice");
    }
  }
}

int main() {
  std::printf("=== DictLayout host litmus ===\n");
  testSpanFitsOneLine();
  testWordWrap();
  testNewlineBeforeStartsLine();
  testStyleAndListIndent();
  testAdjacentSameStyleMerges();
  testPaginate();
  testPageCollectorMatchesOracle();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
