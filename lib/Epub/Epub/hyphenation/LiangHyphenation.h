#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"
#include "SerializedHyphenationTrie.h"

// Encapsulates every language-specific dial the Liang algorithm needs at runtime.  The helpers are
// intentionally represented as bare function pointers because we invoke them inside tight loops and
// want to avoid the overhead of std::function or functors.  The minima default to the TeX-recommended
// "2/2" split but individual languages (English, for example) can override them.
struct LiangWordConfig {
  static constexpr size_t kDefaultMinPrefix = 2;
  static constexpr size_t kDefaultMinSuffix = 2;
  // Predicate used to reject non-alphabetic characters before pattern lookup.  Returning false causes
  // the entire word to be skipped, matching the behavior of classic TeX hyphenation tables.
  bool (*isLetter)(uint32_t);
  // Language-specific case folding that matches how the TeX patterns were authored (usually lower-case
  // ASCII for Latin and lowercase Cyrillic for Russian).  Patterns are stored in UTF-8, so this must
  // operate on Unicode scalars rather than bytes.
  uint32_t (*toLower)(uint32_t);
  // Minimum codepoints required on the left/right of any break.  These correspond to TeX's
  // lefthyphenmin and righthyphenmin knobs.
  size_t minPrefix;
  size_t minSuffix;

  // Lightweight aggregate constructor so call sites can declare `const LiangWordConfig config(...)`
  // without verbose member assignment boilerplate.
  LiangWordConfig(bool (*letterFn)(uint32_t), uint32_t (*lowerFn)(uint32_t), size_t prefix = kDefaultMinPrefix,
                  size_t suffix = kDefaultMinSuffix)
      : isLetter(letterFn), toLower(lowerFn), minPrefix(prefix), minSuffix(suffix) {}
};

// Shared Liang pattern evaluator used by every language-specific hyphenator.
std::vector<size_t> liangBreakIndexes(const std::vector<CodepointInfo>& cps,
                                      const SerializedHyphenationPatterns& patterns, const LiangWordConfig& config);
