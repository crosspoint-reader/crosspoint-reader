#include "DictLayout.h"

#include <Utf8.h>

#include <numeric>
#include <utility>

#include "DictFontUtils.h"

namespace DictLayout {

Wrapper::Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink)
    : maxWidth_(metrics.maxWidth),
      indentStep_(metrics.indentStep),
      bulletWidth_(metrics.bulletWidth),
      measure_(measure),
      sink_(sink) {
  dictRuns_.reserve(4);
  startLine(0, false);
}

int getMixedWidth(std::vector<DictTextSpan>& scratchRuns, const char* text, EpdFontFamily::Style style,
                  const Measurer& measure) {
  scratchRuns.clear();
  splitDictRuns(text, scratchRuns);
  return std::accumulate(scratchRuns.begin(), scratchRuns.end(), 0, [&](int sum, const DictTextSpan& run) {
    return sum + measure(run.text.c_str(), style, run.isDictFont);
  });
}

void Wrapper::flushLine() {
  if (!currentLine_.segments.empty()) {
    sink_(std::move(currentLine_));
    currentLine_ = LayoutLine{};
  }
}

void Wrapper::startLine(uint8_t indent, bool listItem) {
  currentLine_.indentLevel = indent;
  currentLine_.isListItem = listItem;
  currentX_ = indent * indentStep_ + (listItem ? bulletWidth_ : 0);
}

void Wrapper::appendToLine(const std::string& text, EpdFontFamily::Style style, bool isDictFont, int width) {
  if (!currentLine_.segments.empty() && currentLine_.segments.back().style == style &&
      currentLine_.segments.back().isDictFont == isDictFont) {
    currentLine_.segments.back().text += text;
  } else {
    currentLine_.segments.push_back({text, style, isDictFont});
  }
  currentX_ += width;
}

void Wrapper::appendMixed(const char* text, EpdFontFamily::Style style) {
  dictRuns_.clear();
  splitDictRuns(text, dictRuns_);
  for (const auto& run : dictRuns_) {
    appendToLine(run.text, style, run.isDictFont, measure_(run.text.c_str(), style, run.isDictFont));
  }
}

// Break a single token at codepoint boundaries when it is wider than the available line width.
// IPA combining-mark handling (pendingIsIpa) mirrors splitDictRuns and must be preserved.
void Wrapper::breakToken(const std::string& tok, EpdFontFamily::Style style, uint8_t indentLevel) {
  const auto* bp = reinterpret_cast<const uint8_t*>(tok.c_str());
  std::string pending;
  int pendingWidth = 0;
  bool pendingIsIpa = false;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&bp))) {
    const bool combining = utf8IsCombiningMark(cp);
    const bool cpIsIpa = combining ? pendingIsIpa : isDictCodepoint(cp);
    if (pending.empty()) pendingIsIpa = cpIsIpa;
    char buf[4];
    const int cpLen = utf8EncodeCodepoint(cp, buf);
    std::string cpStr(buf, cpLen);
    const int cpWidth = measure_(cpStr.c_str(), style, cpIsIpa);
    if (!pending.empty() && currentX_ + pendingWidth + cpWidth > maxWidth_) {
      appendMixed(pending.c_str(), style);
      flushLine();
      startLine(indentLevel, false);
      pending.clear();
      pendingWidth = 0;
      pendingIsIpa = cpIsIpa;
    }
    pending += cpStr;
    pendingWidth += cpWidth;
  }
  if (!pending.empty()) appendMixed(pending.c_str(), style);
}

void Wrapper::onSpan(const StyledSpan& span) {
  if (!span.text || span.text[0] == '\0') return;

  EpdFontFamily::Style style;
  if (span.bold && span.italic) {
    style = EpdFontFamily::BOLD_ITALIC;
  } else if (span.bold) {
    style = EpdFontFamily::BOLD;
  } else if (span.italic) {
    style = EpdFontFamily::ITALIC;
  } else {
    style = EpdFontFamily::REGULAR;
  }
  if (span.underline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);

  if (span.newlineBefore) {
    flushLine();
    startLine(span.indentLevel, span.isListItem);
  }

  const int spanWidth = getMixedWidth(dictRuns_, span.text, style, measure_);
  if (currentX_ + spanWidth <= maxWidth_) {
    // Fast path: entire span fits on the current line.
    appendMixed(span.text, style);
  } else {
    // Word-wrap within the span.
    const char* p = span.text;
    while (*p) {
      bool hadSpace = false;
      while (*p == ' ') {
        hadSpace = true;
        ++p;
      }
      if (!*p) {
        if (hadSpace) {
          const int spaceWidth = measure_(" ", style, false);
          if (currentX_ + spaceWidth <= maxWidth_) {
            appendToLine(" ", style, false, spaceWidth);
          }
        }
        break;
      }

      const char* tokStart = p;
      while (*p && *p != ' ') ++p;
      const std::string tok(tokStart, p - tokStart);

      bool lineIsEmpty = currentLine_.segments.empty();
      bool useSpace = !lineIsEmpty && hadSpace;
      const int tokWidth = getMixedWidth(dictRuns_, tok.c_str(), style, measure_);
      const int spaceWidth = useSpace ? measure_(" ", style, false) : 0;

      if (currentX_ + spaceWidth + tokWidth > maxWidth_ && !lineIsEmpty) {
        flushLine();
        startLine(span.indentLevel, false);
        useSpace = false;
      }

      if (currentX_ + (useSpace ? spaceWidth : 0) + tokWidth > maxWidth_) {
        breakToken(tok, style, span.indentLevel);
      } else {
        if (useSpace) appendToLine(" ", style, false, spaceWidth);
        appendMixed(tok.c_str(), style);
      }
    }
  }
}

void Wrapper::finish() { flushLine(); }

// --------------------------------------------------------------------------
// Free-function drivers over Wrapper
// --------------------------------------------------------------------------

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink) {
  Wrapper wrapper(metrics, measure, sink);
  for (const auto& span : spans) wrapper.onSpan(span);
  wrapper.finish();
}

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out) {
  out.clear();
  out.reserve(32);
  const LineSink sink{&out, [](void* ctx, LayoutLine&& line) {
                        static_cast<std::vector<LayoutLine>*>(ctx)->push_back(std::move(line));
                      }};
  wrapSpans(spans, metrics, measure, sink);
}

}  // namespace DictLayout
