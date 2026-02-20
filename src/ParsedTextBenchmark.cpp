// *** BENCHMARK ONLY — compiled only when ENABLE_PARSEDTEXT_BENCHMARK is defined ***
#ifdef ENABLE_PARSEDTEXT_BENCHMARK

#include "ParsedTextBenchmark.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <Epub/ParsedText.h>
#include <Epub/ParsedTextLegacy.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Number of full layout passes per variant.  50 is enough for stable ms figures
// without hanging the watchdog during boot.
static constexpr int BENCHMARK_ITERATIONS = 50;

// Typical 6-inch e-ink viewport minus left+right margins used by the reader.
static constexpr uint16_t VIEWPORT_WIDTH = 474;

// ---------------------------------------------------------------------------
// Test corpus
// ---------------------------------------------------------------------------
// Represents one word as the parser would feed it: the text, which font style
// weight, and whether it is attached to the previous token (no space before it,
// as happens for closing punctuation mid-sentence).

struct WordEntry {
  const char* text;
  EpdFontFamily::Style style;
  bool attachToPrevious;
};

// A realistic two-sentence epub paragraph with mixed bold/italic runs and
// sentence-final punctuation attached to the last word.  About 65 tokens —
// typical for a mid-length paragraph from a novel.
// clang-format off
static const WordEntry TEST_WORDS[] = {
  // "It was the best of times, it was the worst of times, it was the age of wisdom..."
  // (A Tale of Two Cities — public domain)
  { "It",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "best",           EpdFontFamily::BOLD,      false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "times,",         EpdFontFamily::REGULAR,   false },
  { "it",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "worst",          EpdFontFamily::BOLD,      false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "times,",         EpdFontFamily::REGULAR,   false },
  { "it",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "age",            EpdFontFamily::REGULAR,   false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "wisdom,",        EpdFontFamily::ITALIC,    false },
  { "it",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "age",            EpdFontFamily::REGULAR,   false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "foolishness,",   EpdFontFamily::ITALIC,    false },
  { "it",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "epoch",          EpdFontFamily::REGULAR,   false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "belief,",        EpdFontFamily::REGULAR,   false },
  { "it",             EpdFontFamily::REGULAR,   false },
  { "was",            EpdFontFamily::REGULAR,   false },
  { "the",            EpdFontFamily::REGULAR,   false },
  { "epoch",          EpdFontFamily::REGULAR,   false },
  { "of",             EpdFontFamily::REGULAR,   false },
  { "incredulity.",   EpdFontFamily::REGULAR,   false },
  // Second sentence — longer words that hyphenation will want to break.
  // "Extraordinary circumstances demanded an unprecedented and extraordinarily
  //  courageous demonstration of philosophical determination."
  { "Extraordinary",        EpdFontFamily::REGULAR,        false },
  { "circumstances",        EpdFontFamily::REGULAR,        false },
  { "demanded",             EpdFontFamily::REGULAR,        false },
  { "an",                   EpdFontFamily::REGULAR,        false },
  { "unprecedented",        EpdFontFamily::BOLD,           false },
  { "and",                  EpdFontFamily::REGULAR,        false },
  { "extraordinarily",      EpdFontFamily::REGULAR,        false },
  { "courageous",           EpdFontFamily::ITALIC,         false },
  { "demonstration",        EpdFontFamily::REGULAR,        false },
  { "of",                   EpdFontFamily::REGULAR,        false },
  { "philosophical",        EpdFontFamily::REGULAR,        false },
  { "determination",        EpdFontFamily::BOLD,           false },
  { "and",                  EpdFontFamily::REGULAR,        false },
  { "unwavering",           EpdFontFamily::REGULAR,        false },
  { "perseverance",         EpdFontFamily::REGULAR,        false },
  { "throughout",           EpdFontFamily::REGULAR,        false },
  { "the",                  EpdFontFamily::REGULAR,        false },
  { "unimaginably",         EpdFontFamily::ITALIC,         false },
  { "challenging",          EpdFontFamily::REGULAR,        false },
  { "circumstances",        EpdFontFamily::REGULAR,        false },
  { "of",                   EpdFontFamily::REGULAR,        false },
  { "their",                EpdFontFamily::REGULAR,        false },
  // Inline superscript-style continuation (e.g. footnote marker glued to word)
  { "remarkable",           EpdFontFamily::BOLD_ITALIC,    false },
  { "1",                    EpdFontFamily::REGULAR,        true  },  // footnote marker
  { "situation.",           EpdFontFamily::REGULAR,        false },
};
// clang-format on

static constexpr size_t TEST_WORD_COUNT = sizeof(TEST_WORDS) / sizeof(TEST_WORDS[0]);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Populate a fresh ParsedText with the test corpus.
static void populateNew(ParsedText& pt) {
  for (size_t i = 0; i < TEST_WORD_COUNT; ++i) {
    pt.addWord(TEST_WORDS[i].text, TEST_WORDS[i].style, /*underline=*/false, TEST_WORDS[i].attachToPrevious);
  }
}

// Populate a fresh ParsedTextLegacy with the same test corpus.
static void populateLegacy(ParsedTextLegacy& pt) {
  for (size_t i = 0; i < TEST_WORD_COUNT; ++i) {
    pt.addWord(TEST_WORDS[i].text, TEST_WORDS[i].style, /*underline=*/false, TEST_WORDS[i].attachToPrevious);
  }
}

// ---------------------------------------------------------------------------
// Single-variant benchmark runner
//
// Returns: { totalMicros, linesProduced }
// heapBefore / heapAfter (out params) capture free-heap around ALL iterations
// so we see the aggregate high-water impact.
// ---------------------------------------------------------------------------

struct BenchResult {
  unsigned long totalUs;  // wall-clock microseconds across all iterations
  int lineCount;          // lines from the *last* iteration (correctness check)
  uint32_t heapBefore;   // free heap bytes before first iteration
  uint32_t heapAfter;    // free heap bytes after last iteration
};

static BenchResult runNew(const GfxRenderer& renderer, int fontId, bool hyphenation) {
  BenchResult r{};
  r.heapBefore = ESP.getFreeHeap();

  for (int iter = 0; iter < BENCHMARK_ITERATIONS; ++iter) {
    BlockStyle bs;
    bs.alignment = CssTextAlign::Justify;

    ParsedText pt(/*extraParagraphSpacing=*/false, hyphenation, bs);
    populateNew(pt);

    int lineCount = 0;
    const unsigned long t0 = micros();
    pt.layoutAndExtractLines(renderer, fontId, VIEWPORT_WIDTH,
                             [&](std::shared_ptr<TextBlock>) { ++lineCount; });
    r.totalUs += micros() - t0;

    if (iter == BENCHMARK_ITERATIONS - 1) r.lineCount = lineCount;
  }

  r.heapAfter = ESP.getFreeHeap();
  return r;
}

static BenchResult runLegacy(const GfxRenderer& renderer, int fontId, bool hyphenation) {
  BenchResult r{};
  r.heapBefore = ESP.getFreeHeap();

  for (int iter = 0; iter < BENCHMARK_ITERATIONS; ++iter) {
    BlockStyle bs;
    bs.alignment = CssTextAlign::Justify;

    ParsedTextLegacy pt(/*extraParagraphSpacing=*/false, hyphenation, bs);
    populateLegacy(pt);

    int lineCount = 0;
    const unsigned long t0 = micros();
    pt.layoutAndExtractLines(renderer, fontId, VIEWPORT_WIDTH,
                             [&](std::shared_ptr<TextBlock>) { ++lineCount; });
    r.totalUs += micros() - t0;

    if (iter == BENCHMARK_ITERATIONS - 1) r.lineCount = lineCount;
  }

  r.heapAfter = ESP.getFreeHeap();
  return r;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void runParsedTextBenchmark(const GfxRenderer& renderer, const int fontId) {
  LOG_INF("BENCH", "=== ParsedText benchmark  (%d iterations, %d words, viewport %dpx) ===",
          BENCHMARK_ITERATIONS, (int)TEST_WORD_COUNT, (int)VIEWPORT_WIDTH);

  // --- Test 1: justified layout, NO hyphenation (DP path) ---
  const BenchResult legacyNH = runLegacy(renderer, fontId, /*hyphenation=*/false);
  const BenchResult newNH    = runNew   (renderer, fontId, /*hyphenation=*/false);

  const long deltaUsNH = static_cast<long>(newNH.totalUs) - static_cast<long>(legacyNH.totalUs);
  const int pctNH =
      legacyNH.totalUs > 0 ? static_cast<int>(100L * deltaUsNH / static_cast<long>(legacyNH.totalUs)) : 0;

  LOG_INF("BENCH", "--- No-hyphenation (DP layout) ---");
  LOG_INF("BENCH", "  Legacy : %6lu us total  (%4lu us/iter)  lines=%d  heapDelta=%+d",
          legacyNH.totalUs, legacyNH.totalUs / BENCHMARK_ITERATIONS,
          legacyNH.lineCount,
          (int)legacyNH.heapAfter - (int)legacyNH.heapBefore);
  LOG_INF("BENCH", "  New    : %6lu us total  (%4lu us/iter)  lines=%d  heapDelta=%+d",
          newNH.totalUs, newNH.totalUs / BENCHMARK_ITERATIONS,
          newNH.lineCount,
          (int)newNH.heapAfter - (int)newNH.heapBefore);
  LOG_INF("BENCH", "  Delta  : %+ld us total  (%+d%%)  %s",
          deltaUsNH, pctNH, pctNH <= 0 ? "IMPROVED" : "REGRESSION");
  if (newNH.lineCount != legacyNH.lineCount) {
    LOG_ERR("BENCH", "  *** LINE COUNT MISMATCH: legacy=%d vs new=%d ***",
            legacyNH.lineCount, newNH.lineCount);
  }

  // --- Test 2: justified layout, WITH hyphenation (greedy path) ---
  const BenchResult legacyH = runLegacy(renderer, fontId, /*hyphenation=*/true);
  const BenchResult newH    = runNew   (renderer, fontId, /*hyphenation=*/true);

  const long deltaUsH = static_cast<long>(newH.totalUs) - static_cast<long>(legacyH.totalUs);
  const int pctH =
      legacyH.totalUs > 0 ? static_cast<int>(100L * deltaUsH / static_cast<long>(legacyH.totalUs)) : 0;

  LOG_INF("BENCH", "--- Hyphenation enabled (greedy layout) ---");
  LOG_INF("BENCH", "  Legacy : %6lu us total  (%4lu us/iter)  lines=%d  heapDelta=%+d",
          legacyH.totalUs, legacyH.totalUs / BENCHMARK_ITERATIONS,
          legacyH.lineCount,
          (int)legacyH.heapAfter - (int)legacyH.heapBefore);
  LOG_INF("BENCH", "  New    : %6lu us total  (%4lu us/iter)  lines=%d  heapDelta=%+d",
          newH.totalUs, newH.totalUs / BENCHMARK_ITERATIONS,
          newH.lineCount,
          (int)newH.heapAfter - (int)newH.heapBefore);
  LOG_INF("BENCH", "  Delta  : %+ld us total  (%+d%%)  %s",
          deltaUsH, pctH, pctH <= 0 ? "IMPROVED" : "REGRESSION");
  if (newH.lineCount != legacyH.lineCount) {
    LOG_ERR("BENCH", "  *** LINE COUNT MISMATCH: legacy=%d vs new=%d ***",
            legacyH.lineCount, newH.lineCount);
  }

  LOG_INF("BENCH", "=== benchmark complete ===");
}

#endif  // ENABLE_PARSEDTEXT_BENCHMARK
