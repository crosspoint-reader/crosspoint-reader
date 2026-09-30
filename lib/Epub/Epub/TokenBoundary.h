#pragma once

#include <cstdint>

#include "hyphenation/HyphenationCommon.h"

// ParsedText stores each boundary in two bit-packed vectors. Together the bits distinguish
// ordinary word gaps, CJK-style stretchable zero-width gaps, unbreakable attachment, and
// breakable attachment after visible hyphens/dashes.
namespace TokenBoundary {

// A continuation normally forbids a line break. noSpaceBefore marks the fourth state where the
// text stays attached when unbroken but the line breaker may still stop at the boundary.
constexpr bool allowsBreak(const bool continues, const bool noSpaceBefore) { return !continues || noSpaceBefore; }

// Ordinary word gaps and CJK zero-width gaps participate in justification. Attached text does
// not, except for a literal space token such as a non-breaking space represented as content.
constexpr bool isJustifiableGap(const bool continues, const bool noSpaceBefore, const bool isSpaceToken) {
  return !continues || (isSpaceToken && !noSpaceBefore);
}

// Soft hyphens become visible only when their conditional break is taken. U+2011 exists to forbid
// a break. Every other character classified by isExplicitHyphen is a visible break opportunity.
inline bool allowsBreakAfterExplicitHyphen(const uint32_t cp) {
  constexpr uint32_t NON_BREAKING_HYPHEN_CP = 0x2011;
  return isExplicitHyphen(cp) && !isSoftHyphen(cp) && cp != NON_BREAKING_HYPHEN_CP;
}

}  // namespace TokenBoundary
