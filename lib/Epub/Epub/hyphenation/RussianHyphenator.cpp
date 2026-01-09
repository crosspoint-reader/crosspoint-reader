#include "RussianHyphenator.h"

#include <vector>

#include "LiangHyphenation.h"
#include "generated/hyph-ru.trie.h"

const RussianHyphenator& RussianHyphenator::instance() {
  static RussianHyphenator instance;
  return instance;
}

std::vector<size_t> RussianHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  // Russian uses the same Liang runtime but needs Cyrillic-aware helpers plus symmetrical
  // lefthyphenmin/righthyphenmin values.  Most Russian TeX distributions stick with 2/2, which keeps
  // short words readable while still allowing frequent hyphenation opportunities.
  const LiangWordConfig config(isCyrillicLetter, toLowerCyrillic, minPrefix(), minSuffix());
  return liangBreakIndexes(cps, ru_ru_patterns, config);
}
