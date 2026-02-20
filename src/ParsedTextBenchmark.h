#pragma once
// *** BENCHMARK ONLY — enabled by -D ENABLE_PARSEDTEXT_BENCHMARK in platformio.ini ***
//
// runParsedTextBenchmark() runs both the current (optimised) and the legacy
// ParsedText implementations over the same input text, BENCHMARK_ITERATIONS
// times each, and logs a comparison table via LOG_INF.
//
// Call it once at the end of setup(), after setupDisplayAndFonts(), e.g.:
//   #ifdef ENABLE_PARSEDTEXT_BENCHMARK
//   runParsedTextBenchmark(renderer, BOOKERLY_14_FONT_ID);
//   #endif

#ifdef ENABLE_PARSEDTEXT_BENCHMARK

class GfxRenderer;

void runParsedTextBenchmark(const GfxRenderer& renderer, int fontId);

#endif  // ENABLE_PARSEDTEXT_BENCHMARK
