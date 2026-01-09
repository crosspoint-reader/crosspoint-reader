#pragma once

#include "LanguageHyphenator.h"

// Implements Liang hyphenation rules for French (Latin script).
class FrenchHyphenator final : public LanguageHyphenator {
 public:
  static const FrenchHyphenator& instance();

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const override;

 private:
  FrenchHyphenator() = default;
};
