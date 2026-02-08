#include <chrono>
#include <iostream>
#include <string>

#include "lib/Epub/Epub/css/CssParser.h"

int main() {
  const std::string simple = "font-weight: bold";
  const std::string medium =
      "font-weight: bold; text-align: center; margin-top: 10px; "
      "padding-left: 2em; font-style: italic";
  const std::string complex =
      "font-weight: bold; text-align: center; margin-top: 10px; "
      "margin-bottom: 20px; margin-left: 5px; margin-right: 5px; "
      "padding-top: 3px; padding-bottom: 3px; padding-left: 2em; "
      "padding-right: 2em; text-indent: 1.5em; "
      "font-style: italic; text-decoration: underline; "
      "text-decoration-line: underline; font-weight: 700";

  struct Bench {
    const char* label;
    const std::string& css;
  };

  Bench benches[] = {
      {"1 property ", simple},
      {"5 properties", medium},
      {"15 properties", complex},
  };

  const int iterations = 100000;
  volatile bool sink = false;  // prevent dead-code elimination

  std::cout << "CssParser::parseInlineStyle benchmark:\n";
  for (const auto& bench : benches) {
    // Warm up
    for (int i = 0; i < 100; ++i) {
      auto s = CssParser::parseInlineStyle(bench.css);
      sink = s.defined.anySet();
    }

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
      auto s = CssParser::parseInlineStyle(bench.css);
      sink = s.defined.anySet();
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "  " << bench.label << ": " << (totalUs * 1000 / iterations) << " ns/call"
              << " (" << iterations << " iterations, " << totalUs << " us total)\n";
  }

  return 0;
}
