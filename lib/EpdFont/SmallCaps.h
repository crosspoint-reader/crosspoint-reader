#pragma once
#include <Utf8.h>

#include <cstdint>

// Shared helpers for font-variant: small-caps rendering.
//
// Small-caps folds lowercase letters to their uppercase glyph and draws them at a
// reduced scale; characters that are not lowercase letters (already-uppercase,
// digits, punctuation) render at full size.  The fold and the scale must be applied
// identically in every measurement and rendering path so layout and drawing agree.
namespace smallCaps {

// Scale factor applied to folded (was-lowercase) glyphs.  ~0.75 keeps small caps
// visibly smaller than full capitals while staying legible on e-ink.
inline constexpr float SCALE = 0.75f;

// Fold a codepoint to its uppercase form for small-caps rendering.
// Returns true and rewrites `cp` to the uppercase codepoint when the input was a
// lowercase letter (and therefore should be drawn scaled); returns false and leaves
// `cp` unchanged otherwise.
//
// Coverage is utf8UpperCodepoint()'s, the inverse of the lowercase mapping the
// hyphenators and the dictionary use: Latin (including the odd-aligned Ĺ-ň and Ź-ž
// runs of Latin Extended-A, ÿ, Vietnamese, Romanian), monotonic Greek with final ς,
// and Cyrillic. A letter whose capital is not one codepoint (ß, ŉ) stays full-size.
inline bool fold(uint32_t& cp) {
  const uint32_t upper = utf8UpperCodepoint(cp);
  if (upper == cp) return false;
  cp = upper;
  return true;
}

}  // namespace smallCaps
