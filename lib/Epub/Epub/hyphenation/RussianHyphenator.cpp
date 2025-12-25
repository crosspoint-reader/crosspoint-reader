#include "RussianHyphenator.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace {

// Checks if the codepoint is the Cyrillic soft sign (ь).
bool isSoftSign(uint32_t cp) { return toLowerCyrillic(cp) == 0x044C; }

// Checks if the codepoint is the Cyrillic hard sign (ъ).
bool isHardSign(uint32_t cp) { return toLowerCyrillic(cp) == 0x044A; }

// Checks if the codepoint is either the Cyrillic soft sign (ь) or hard sign (ъ).
bool isSoftOrHardSign(uint32_t cp) { return isSoftSign(cp) || isHardSign(cp); }

// Checks if the codepoint is the Cyrillic short i (й).
bool isCyrillicShortI(uint32_t cp) { return toLowerCyrillic(cp) == 0x0439; }

// Checks if the codepoint is the Cyrillic yeru (ы).
bool isCyrillicYeru(uint32_t cp) { return toLowerCyrillic(cp) == 0x044B; }

// Checks if the codepoint is a Russian prefix consonant that can start certain clusters.
bool isRussianPrefixConsonant(uint32_t cp) {
  cp = toLowerCyrillic(cp);
  return cp == 0x0432 || cp == 0x0437 || cp == 0x0441;  // в, з, с
}

// Checks if the codepoint is a Russian sibilant consonant.
bool isRussianSibilant(uint32_t cp) {
  cp = toLowerCyrillic(cp);
  switch (cp) {
    case 0x0437:  // з
    case 0x0441:  // с
    case 0x0436:  // ж
    case 0x0448:  // ш
    case 0x0449:  // щ
    case 0x0447:  // ч
    case 0x0446:  // ц
      return true;
    default:
      return false;
  }
}

// Checks if the codepoint is a Russian stop consonant.
bool isRussianStop(uint32_t cp) {
  cp = toLowerCyrillic(cp);
  switch (cp) {
    case 0x0431:  // б
    case 0x0433:  // г
    case 0x0434:  // д
    case 0x043F:  // п
    case 0x0442:  // т
    case 0x043A:  // к
      return true;
    default:
      return false;
  }
}

// Checks the sonority rank of a Russian consonant for syllable onset validation.
int russianSonority(uint32_t cp) {
  cp = toLowerCyrillic(cp);
  switch (cp) {
    case 0x043B:  // л
    case 0x0440:  // р
    case 0x0439:  // й
      return 4;
    case 0x043C:  // м
    case 0x043D:  // н
      return 3;
    case 0x0432:  // в
    case 0x0437:  // з
    case 0x0436:  // ж
      return 2;
    case 0x0444:  // ф
    case 0x0441:  // с
    case 0x0448:  // ш
    case 0x0449:  // щ
    case 0x0447:  // ч
    case 0x0446:  // ц
    case 0x0445:  // х
      return 1;
    case 0x0431:  // б
    case 0x0433:  // г
    case 0x0434:  // д
    case 0x043F:  // п
    case 0x0442:  // т
    case 0x043A:  // к
      return 0;
    default:
      return 1;
  }
}

// Applies Russian sonority sequencing to ensure the consonant cluster can start a syllable.
bool russianClusterIsValidOnset(const std::vector<CodepointInfo>& cps, const size_t start, const size_t end) {
  if (start >= end) {
    return false;
  }

  for (size_t i = start; i < end; ++i) {
    const auto cp = cps[i].value;
    if (!isCyrillicConsonant(cp) || isSoftOrHardSign(cp)) {
      return false;
    }
  }

  if (end - start == 1) {
    return true;
  }

  for (size_t i = start; i + 1 < end; ++i) {
    const uint32_t current = cps[i].value;
    const uint32_t next = cps[i + 1].value;
    const int currentRank = russianSonority(current);
    const int nextRank = russianSonority(next);
    if (currentRank > nextRank) {
      const bool atClusterStart = (i == start);
      const bool prefixAllowance = atClusterStart && isRussianPrefixConsonant(current);
      const bool sibilantAllowance = isRussianSibilant(current) && isRussianStop(next);
      if (!prefixAllowance && !sibilantAllowance) {
        return false;
      }
    }
  }

  return true;
}

// Identifies splits within double consonant clusters.
size_t doubleConsonantSplit(const std::vector<CodepointInfo>& cps, const size_t clusterStart, const size_t clusterEnd) {
  for (size_t i = clusterStart; i + 1 < clusterEnd; ++i) {
    const auto left = cps[i].value;
    const auto right = cps[i + 1].value;
    if (isCyrillicConsonant(left) && toLowerCyrillic(left) == toLowerCyrillic(right) && !isSoftOrHardSign(right)) {
      return i + 1;
    }
  }
  return std::numeric_limits<size_t>::max();
}

// Prevents breaks that would create forbidden suffixes.
bool beginsWithForbiddenSuffix(const std::vector<CodepointInfo>& cps, const size_t index) {
  if (index >= cps.size()) {
    return true;
  }
  const auto cp = cps[index].value;
  return isSoftOrHardSign(cp) || isCyrillicShortI(cp) || isCyrillicYeru(cp);
}

// Validates whether a hyphenation break is allowed at the specified index.
bool russianBreakAllowed(const std::vector<CodepointInfo>& cps, const size_t breakIndex) {
  if (breakIndex == 0 || breakIndex >= cps.size()) {
    return false;
  }

  const size_t prefixLen = breakIndex;
  const size_t suffixLen = cps.size() - breakIndex;
  if (prefixLen < 2 || suffixLen < 2) {
    return false;
  }

  if (beginsWithForbiddenSuffix(cps, breakIndex)) {
    return false;
  }

  return true;
}

// Chooses the longest valid onset contained within the inter-vowel cluster.
size_t russianOnsetLength(const std::vector<CodepointInfo>& cps, const size_t clusterStart, const size_t clusterEnd) {
  const size_t clusterLen = clusterEnd - clusterStart;
  if (clusterLen == 0) {
    return 0;
  }

  const size_t maxLen = std::min<size_t>(4, clusterLen);
  for (size_t len = maxLen; len >= 1; --len) {
    const size_t suffixStart = clusterEnd - len;
    if (russianClusterIsValidOnset(cps, suffixStart, clusterEnd)) {
      return len;
    }
  }

  return 1;
}

// Prevents hyphenation splits immediately beside ь/ъ characters.
bool nextToSoftSign(const std::vector<CodepointInfo>& cps, const size_t index) {
  if (index == 0 || index >= cps.size()) {
    return false;
  }
  const auto left = cps[index - 1].value;
  const auto right = cps[index].value;
  return isSoftOrHardSign(left) || isSoftOrHardSign(right);
}

// Produces syllable break indexes tailored to Russian phonotactics.
std::vector<size_t> russianBreakIndexes(const std::vector<CodepointInfo>& cps) {
  std::vector<size_t> indexes;
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return indexes;
  }

  std::vector<size_t> vowelPositions;
  vowelPositions.reserve(cps.size());
  for (size_t i = 0; i < cps.size(); ++i) {
    if (isCyrillicVowel(cps[i].value)) {
      vowelPositions.push_back(i);
    }
  }

  if (vowelPositions.size() < 2) {
    return indexes;
  }

  for (size_t v = 0; v + 1 < vowelPositions.size(); ++v) {
    const size_t leftVowel = vowelPositions[v];
    const size_t rightVowel = vowelPositions[v + 1];

    if (rightVowel - leftVowel == 1) {
      if (rightVowel >= MIN_PREFIX_CP && cps.size() - rightVowel >= MIN_SUFFIX_CP && !nextToSoftSign(cps, rightVowel) &&
          russianBreakAllowed(cps, rightVowel)) {
        indexes.push_back(rightVowel);
      }
      continue;
    }

    const size_t clusterStart = leftVowel + 1;
    const size_t clusterEnd = rightVowel;

    size_t breakIndex = std::numeric_limits<size_t>::max();
    if (const auto split = doubleConsonantSplit(cps, clusterStart, clusterEnd);
        split != std::numeric_limits<size_t>::max()) {
      breakIndex = split;
    } else {
      const size_t onsetLen = russianOnsetLength(cps, clusterStart, clusterEnd);
      breakIndex = clusterEnd - onsetLen;
    }

    if (breakIndex == std::numeric_limits<size_t>::max()) {
      continue;
    }

    if (breakIndex < MIN_PREFIX_CP || cps.size() - breakIndex < MIN_SUFFIX_CP) {
      continue;
    }
    if (nextToSoftSign(cps, breakIndex)) {
      continue;
    }
    if (!russianBreakAllowed(cps, breakIndex)) {
      continue;
    }
    indexes.push_back(breakIndex);
  }

  std::sort(indexes.begin(), indexes.end());
  indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
  return indexes;
}

}  // namespace

const RussianHyphenator& RussianHyphenator::instance() {
  static RussianHyphenator instance;
  return instance;
}

Script RussianHyphenator::script() const { return Script::Cyrillic; }

std::vector<size_t> RussianHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  return russianBreakIndexes(cps);
}
