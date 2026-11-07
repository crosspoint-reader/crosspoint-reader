#pragma once

namespace selectionAction {
// Selections up to this many words default to look-up in the action menu; longer ones default to clip.
constexpr int MAX_LOOKUP_WORDS = 2;

enum class Kind { Lookup, Clip };

// A single word runs look-up straight away (no menu); without a dictionary the only action left is clip.
inline bool runsWithoutMenu(const int wordCount, const bool lookupAvailable) {
  return !lookupAvailable || wordCount <= 1;
}

inline Kind defaultAction(const int wordCount, const bool lookupAvailable) {
  return lookupAvailable && wordCount <= MAX_LOOKUP_WORDS ? Kind::Lookup : Kind::Clip;
}
}  // namespace selectionAction
