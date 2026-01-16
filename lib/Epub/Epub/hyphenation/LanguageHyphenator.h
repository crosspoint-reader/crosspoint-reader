#pragma once

#include "LiangHyphenation.h"

// Generic Liang-backed hyphenator that stores pattern metadata plus language-specific helpers.
class LanguageHyphenator {
 public:
  LanguageHyphenator(const SerializedHyphenationPatterns& patterns, bool (*isLetterFn)(uint32_t),
                     uint32_t (*toLowerFn)(uint32_t), size_t minPrefix = LiangWordConfig::kDefaultMinPrefix,
                     size_t minSuffix = LiangWordConfig::kDefaultMinSuffix)
      : patterns_(patterns), config_(isLetterFn, toLowerFn, minPrefix, minSuffix) {}

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const {
    return liangBreakIndexes(cps, patterns_, config_);
  }

  size_t minPrefix() const { return config_.minPrefix; }
  size_t minSuffix() const { return config_.minSuffix; }

 protected:
  const SerializedHyphenationPatterns& patterns_;
  LiangWordConfig config_;
};
