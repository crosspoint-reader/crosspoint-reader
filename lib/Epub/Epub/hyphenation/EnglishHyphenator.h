#pragma once

#include "LanguageHyphenator.h"

class EnglishHyphenator final : public LanguageHyphenator {
 public:
  static const EnglishHyphenator& instance();

  Script script() const override;
  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const override;

 private:
  EnglishHyphenator() = default;
};
