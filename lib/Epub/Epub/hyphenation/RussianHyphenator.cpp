#include "RussianHyphenator.h"

#include <algorithm>
#include <vector>

namespace {

bool isSoftOrHardSign(const uint32_t cp) {
  return cp == 0x044C || cp == 0x042C || cp == 0x044A || cp == 0x042A;
}

bool isRussianPrefixConsonant(uint32_t cp) {
  cp = toLowerCyrillic(cp);
  return cp == 0x0432 || cp == 0x0437 || cp == 0x0441;  // в, з, с
}

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

bool nextToSoftSign(const std::vector<CodepointInfo>& cps, const size_t index) {
  if (index == 0 || index >= cps.size()) {
    return false;
  }
  const auto left = cps[index - 1].value;
  const auto right = cps[index].value;
  return isSoftOrHardSign(left) || isSoftOrHardSign(right);
}

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
      if (rightVowel >= MIN_PREFIX_CP && cps.size() - rightVowel >= MIN_SUFFIX_CP &&
          !nextToSoftSign(cps, rightVowel)) {
        indexes.push_back(rightVowel);
      }
      continue;
    }

    const size_t clusterStart = leftVowel + 1;
    const size_t clusterEnd = rightVowel;
    const size_t onsetLen = russianOnsetLength(cps, clusterStart, clusterEnd);
    size_t breakIndex = clusterEnd - onsetLen;

    if (breakIndex < MIN_PREFIX_CP || cps.size() - breakIndex < MIN_SUFFIX_CP) {
      continue;
    }
    if (nextToSoftSign(cps, breakIndex)) {
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
