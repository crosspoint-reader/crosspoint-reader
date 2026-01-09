#include "EnglishHyphenator.h"

#include <vector>

#include "LiangHyphenation.h"
#include "generated/hyph-en.trie.h"

const EnglishHyphenator& EnglishHyphenator::instance() {
  static EnglishHyphenator instance;
  return instance;
}

std::vector<size_t> EnglishHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  // The shared Liang engine needs to know which letters are valid, how to lowercase them, and what
  // TeX-style prefix/suffix minima to respect (currently set to lefthyphenmin=2 and righthyphenmin=2)
  const LiangWordConfig config(isLatinLetter, toLowerLatin, minPrefix(), minSuffix());
  return liangBreakIndexes(cps, en_us_patterns, config);
}
