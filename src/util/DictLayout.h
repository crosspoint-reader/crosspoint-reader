#pragma once

#include <DictHtmlRenderer.h>
#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

#include "DictFontUtils.h"  // DictTextSpan (Wrapper scratch member)

// Pure, renderer-independent layout of dictionary HTML spans into wrapped
// display lines. Width measurement is injected via Measurer, so this module has
// no dependency on GfxRenderer / CrossPointSettings / font IDs. That decoupling
// is what lets the wrap/pagination logic be unit-tested host-side with a
// deterministic fake measurer (Tier-A litmus), while the device supplies a
// real font-metric-backed measurer.
namespace DictLayout {

// A single styled run within a display line.
struct LayoutSegment {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool isDictFont = false;  // true → render with the IPA font
};

// One wrapped display line, containing one or more styled segments.
struct LayoutLine {
  std::vector<LayoutSegment> segments;
  uint8_t indentLevel = 0;
  bool isListItem = false;
};

// Width measurement injection. fn returns the pixel width of `text` rendered at
// `style`, using the IPA font when isDictFont is true and the body font otherwise.
// Function-pointer + ctx (NOT std::function) per the project's binary-size rules.
struct Measurer {
  void* ctx = nullptr;
  int (*fn)(void* ctx, const char* text, EpdFontFamily::Style style, bool isDictFont) = nullptr;
  int operator()(const char* text, EpdFontFamily::Style style, bool isDictFont) const {
    return fn(ctx, text, style, isDictFont);
  }
};

// Pixel metrics for one wrap pass. maxWidth is the usable line width; indentStep
// is one indent level in px; bulletWidth is the list-item bullet prefix width.
struct WrapMetrics {
  int maxWidth = 0;
  int indentStep = 0;
  int bulletWidth = 0;
};

// Receives each completed display line as it is produced, in order. The callee
// may keep or discard each line (e.g. retain only one page's worth while counting
// the rest) — wrapSpans always produces every line exactly once. Function-pointer
// + ctx (NOT std::function) per the project's binary-size rules.
struct LineSink {
  void* ctx = nullptr;
  void (*fn)(void* ctx, LayoutLine&& line) = nullptr;
  void operator()(LayoutLine&& line) const { fn(ctx, std::move(line)); }
};

// Stateful word-wrapper. Spans are fed one at a time via onSpan() (so the source
// — the HTML renderer — can stream them and never materialize the whole
// definition), and each completed line is emitted to the LineSink as it is
// finished. finish() flushes the trailing line. Equivalent to the one-shot
// wrapSpans(); that overload is a thin loop over this class.
class Wrapper {
 public:
  Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink);

  void onSpan(const StyledSpan& span);  // process one span; emit completed lines to the sink
  void finish();                        // flush the trailing in-progress line

 private:
  void flushLine();
  void startLine(uint8_t indent, bool listItem);
  void appendToLine(const std::string& text, EpdFontFamily::Style style, bool isDictFont, int width);
  void appendMixed(const char* text, EpdFontFamily::Style style);
  void breakToken(const std::string& tok, EpdFontFamily::Style style, uint8_t indentLevel);

  const int maxWidth_;
  const int indentStep_;
  const int bulletWidth_;
  Measurer measure_;
  LineSink sink_;
  LayoutLine currentLine_;
  int currentX_ = 0;
  std::vector<DictTextSpan> dictRuns_;  // reused scratch for IPA run splitting
};

// Measure the width of a string, accounting for mixed IPA/non-IPA runs.
// Reuses the provided scratchRuns vector to prevent heap allocation overhead.
int getMixedWidth(std::vector<DictTextSpan>& scratchRuns, const char* text, EpdFontFamily::Style style,
                  const Measurer& measure);

// Streaming layout: word-wrap every span and emit each completed line to `sink`
// as soon as it is finished. Lets the caller hold only a subset of lines (one
// page) rather than the whole definition — this is the Stage 2a RAM fix.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink);

// Reference full-layout path: word-wrap every span into `out` (cleared first).
// Produces the entire definition's lines in one pass — the differential oracle
// the streaming path is checked against. Implemented on top of the sink variant.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out);

// Number of pages for a given line count and page size. Always >= 1.
inline int paginate(int lineCount, int linesPerPage) {
  if (linesPerPage < 1) linesPerPage = 1;
  const int pages = (lineCount + linesPerPage - 1) / linesPerPage;
  return pages < 1 ? 1 : pages;
}

}  // namespace DictLayout
