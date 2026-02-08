#include <GfxRenderer.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "lib/Epub/Epub/ParsedText.h"
#include "lib/Epub/Epub/blocks/TextBlock.h"

static void benchLayoutAndExtract(int iterations) {
  GfxRenderer renderer;

  // Pre-build a list of ~200 words (typical paragraph)
  std::vector<std::string> sampleWords;
  for (int i = 0; i < 200; ++i) {
    // Mix of short and medium words
    switch (i % 5) {
      case 0:
        sampleWords.push_back("the");
        break;
      case 1:
        sampleWords.push_back("quick");
        break;
      case 2:
        sampleWords.push_back("brown");
        break;
      case 3:
        sampleWords.push_back("fox");
        break;
      case 4:
        sampleWords.push_back("jumps");
        break;
    }
  }

  auto start = std::chrono::high_resolution_clock::now();

  for (int iter = 0; iter < iterations; ++iter) {
    BlockStyle bs;
    bs.alignment = CssTextAlign::Justify;
    bs.textAlignDefined = true;

    ParsedText text(false, false, bs);
    for (const auto& w : sampleWords) {
      text.addWord(w, EpdFontFamily::REGULAR);
    }

    text.layoutAndExtractLines(renderer, 0, 400, [](std::unique_ptr<TextBlock>) {});
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  auto perCall = elapsed / iterations;

  std::cout << "  " << iterations << " iterations, " << perCall << " ns/call (" << (elapsed / 1000000)
            << " ms total)\n";
}

int main() {
  std::cout << "TextLayoutBench — layoutAndExtractLines with ~200 words\n";
  benchLayoutAndExtract(10000);
  return 0;
}
