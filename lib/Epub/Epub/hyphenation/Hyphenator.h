#pragma once

#include <EpdFontFamily.h>

#include <string>

class GfxRenderer;

struct HyphenationResult {
  std::string head;
  std::string tail;
};

class Hyphenator {
 public:
  static bool splitWord(const GfxRenderer& renderer, int fontId, const std::string& word, EpdFontStyle style,
                        int availableWidth, HyphenationResult* result, bool force);
};