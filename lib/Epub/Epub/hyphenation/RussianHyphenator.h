#pragma once

#include "LanguageHyphenator.h"

class RussianHyphenator final : public LanguageHyphenator {
 public:
  static const RussianHyphenator& instance();

  Script script() const override;
  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const override;

 private:
  RussianHyphenator() = default;
};
