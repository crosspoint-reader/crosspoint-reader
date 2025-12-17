#include "EnglishHyphenator.h"

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace {

char lowerLatinChar(const uint32_t cp) {
  if (!isLatinLetter(cp)) {
    return 0;
  }
  return static_cast<char>(toLowerLatin(cp));
}

bool isEnglishApproximantChar(const char c) { return c == 'l' || c == 'r' || c == 'w' || c == 'y'; }

bool isEnglishStopChar(const char c) {
  switch (c) {
    case 'p':
    case 'b':
    case 't':
    case 'd':
    case 'k':
    case 'g':
    case 'c':
    case 'q':
      return true;
    default:
      return false;
  }
}

bool isEnglishFricativeChar(const char c) {
  switch (c) {
    case 'f':
    case 'v':
    case 's':
    case 'z':
    case 'h':
    case 'x':
      return true;
    default:
      return false;
  }
}

struct CharPair {
  char first;
  char second;
};

bool matchesDigraph(const char first, const char second, const std::initializer_list<CharPair>& pairs) {
  for (const auto& pair : pairs) {
    if (pair.first == first && pair.second == second) {
      return true;
    }
  }
  return false;
}

bool isEnglishDiphthong(const uint32_t first, const uint32_t second) {
  if (!isLatinLetter(first) || !isLatinLetter(second)) {
    return false;
  }
  const auto f = static_cast<char>(toLowerLatin(first));
  const auto s = static_cast<char>(toLowerLatin(second));
  switch (f) {
    case 'a':
      return s == 'i' || s == 'y' || s == 'u';
    case 'e':
      return s == 'a' || s == 'e' || s == 'i' || s == 'o' || s == 'u' || s == 'y';
    case 'i':
      return s == 'e' || s == 'u' || s == 'a';
    case 'o':
      return s == 'a' || s == 'e' || s == 'i' || s == 'o' || s == 'u' || s == 'y';
    case 'u':
      return s == 'i' || s == 'a' || s == 'e';
  }
  return false;
}

bool isValidEnglishOnsetBigram(const uint32_t firstCp, const uint32_t secondCp) {
  const char first = lowerLatinChar(firstCp);
  const char second = lowerLatinChar(secondCp);
  if (!first || !second) {
    return false;
  }

  if (matchesDigraph(first, second,
                     {{'c', 'h'}, {'s', 'h'}, {'t', 'h'}, {'p', 'h'}, {'w', 'h'}, {'w', 'r'}, {'k', 'n'},
                      {'g', 'n'}, {'p', 's'}, {'p', 't'}, {'p', 'n'}, {'r', 'h'}})) {
    return true;
  }

  if (isEnglishStopChar(first) && isEnglishApproximantChar(second)) {
    return true;
  }

  if (isEnglishFricativeChar(first) && isEnglishApproximantChar(second)) {
    return true;
  }

  if (first == 's' && (second == 'p' || second == 't' || second == 'k' || second == 'm' || second == 'n' ||
                       second == 'f' || second == 'l' || second == 'w' || second == 'c')) {
    return true;
  }

  if (second == 'y' && (first == 'p' || first == 'b' || first == 't' || first == 'd' || first == 'f' || first == 'k' ||
                        first == 'g' || first == 'h' || first == 'm' || first == 'n' || first == 'l' || first == 's')) {
    return true;
  }

  return false;
}

bool isValidEnglishOnsetTrigram(const uint32_t firstCp, const uint32_t secondCp, const uint32_t thirdCp) {
  const char first = lowerLatinChar(firstCp);
  const char second = lowerLatinChar(secondCp);
  const char third = lowerLatinChar(thirdCp);
  if (!first || !second || !third) {
    return false;
  }

  if (first == 's') {
    if (second == 'p' && (third == 'l' || third == 'r' || third == 'w')) {
      return true;
    }
    if (second == 't' && (third == 'r' || third == 'w' || third == 'y')) {
      return true;
    }
    if (second == 'k' && (third == 'l' || third == 'r' || third == 'w')) {
      return true;
    }
    if (second == 'c' && (third == 'l' || third == 'r')) {
      return true;
    }
    if (second == 'f' && third == 'r') {
      return true;
    }
    if (second == 'h' && third == 'r') {
      return true;
    }
  }

  if (first == 't' && second == 'h' && third == 'r') {
    return true;
  }

  return false;
}

bool englishClusterIsValidOnset(const std::vector<CodepointInfo>& cps, const size_t start, const size_t end) {
  if (start >= end) {
    return false;
  }

  for (size_t i = start; i < end; ++i) {
    const char ch = lowerLatinChar(cps[i].value);
    if (!ch) {
      return false;
    }
    if (!isLatinConsonant(cps[i].value) && ch != 'y') {
      return false;
    }
  }

  const size_t len = end - start;
  if (len == 1) {
    return true;
  }
  if (len == 2) {
    return isValidEnglishOnsetBigram(cps[start].value, cps[start + 1].value);
  }
  if (len == 3) {
    return isValidEnglishOnsetTrigram(cps[start].value, cps[start + 1].value, cps[start + 2].value);
  }

  return false;
}

size_t englishOnsetLength(const std::vector<CodepointInfo>& cps, const size_t clusterStart, const size_t clusterEnd) {
  const size_t clusterLen = clusterEnd - clusterStart;
  if (clusterLen == 0) {
    return 0;
  }

  const size_t maxLen = std::min<size_t>(3, clusterLen);
  for (size_t len = maxLen; len >= 1; --len) {
    const size_t suffixStart = clusterEnd - len;
    if (englishClusterIsValidOnset(cps, suffixStart, clusterEnd)) {
      return len;
    }
  }

  return 1;
}

bool nextToApostrophe(const std::vector<CodepointInfo>& cps, const size_t index) {
  if (index == 0 || index >= cps.size()) {
    return false;
  }
  const auto left = cps[index - 1].value;
  const auto right = cps[index].value;
  return left == '\'' || right == '\'';
}

std::vector<size_t> englishBreakIndexes(const std::vector<CodepointInfo>& cps) {
  std::vector<size_t> indexes;
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return indexes;
  }

  std::vector<size_t> vowelPositions;
  vowelPositions.reserve(cps.size());
  for (size_t i = 0; i < cps.size(); ++i) {
    if (isLatinVowel(cps[i].value)) {
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
      if (!isEnglishDiphthong(cps[leftVowel].value, cps[rightVowel].value) &&
          rightVowel >= MIN_PREFIX_CP && cps.size() - rightVowel >= MIN_SUFFIX_CP &&
          !nextToApostrophe(cps, rightVowel)) {
        indexes.push_back(rightVowel);
      }
      continue;
    }

    const size_t clusterStart = leftVowel + 1;
    const size_t clusterEnd = rightVowel;
    const size_t onsetLen = englishOnsetLength(cps, clusterStart, clusterEnd);
    size_t breakIndex = clusterEnd - onsetLen;

    if (breakIndex < MIN_PREFIX_CP || cps.size() - breakIndex < MIN_SUFFIX_CP) {
      continue;
    }
    if (nextToApostrophe(cps, breakIndex)) {
      continue;
    }
    indexes.push_back(breakIndex);
  }

  std::sort(indexes.begin(), indexes.end());
  indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
  return indexes;
}

}  // namespace

const EnglishHyphenator& EnglishHyphenator::instance() {
  static EnglishHyphenator instance;
  return instance;
}

Script EnglishHyphenator::script() const { return Script::Latin; }

std::vector<size_t> EnglishHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  return englishBreakIndexes(cps);
}
