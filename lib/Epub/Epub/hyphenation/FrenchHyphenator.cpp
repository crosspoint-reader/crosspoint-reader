#include "FrenchHyphenator.h"

#include <vector>

#include "LiangHyphenation.h"
#include "generated/hyph-fr.trie.h"

const FrenchHyphenator& FrenchHyphenator::instance() {
  static FrenchHyphenator instance;
  return instance;
}

std::vector<size_t> FrenchHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  const LiangWordConfig config(isLatinLetter, toLowerLatin, minPrefix(), minSuffix());
  return liangBreakIndexes(cps, fr_patterns, config);
}
