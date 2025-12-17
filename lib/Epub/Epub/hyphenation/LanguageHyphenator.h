#pragma once

#include <vector>

#include "HyphenationCommon.h"

class LanguageHyphenator {
 public:
  virtual ~LanguageHyphenator() = default;
  virtual Script script() const = 0;
  virtual std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const = 0;
};
