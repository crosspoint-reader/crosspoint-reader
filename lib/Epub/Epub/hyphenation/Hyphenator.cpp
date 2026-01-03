#include "Hyphenator.h"

#include <Utf8.h>

#include <algorithm>
#include <array>
#include <vector>

#include "EnglishHyphenator.h"
#include "HyphenationCommon.h"
#include "LanguageHyphenator.h"
#include "RussianHyphenator.h"

namespace {

// Central registry for language-specific hyphenators supported on device.
const std::array<const LanguageHyphenator*, 2>& registeredHyphenators() {
  static const std::array<const LanguageHyphenator*, 2> hyphenators = {
      &EnglishHyphenator::instance(),
      &RussianHyphenator::instance(),
  };
  return hyphenators;
}

// Finds the hyphenator matching the detected script.
const LanguageHyphenator* hyphenatorForScript(const Script script) {
  for (const auto* hyphenator : registeredHyphenators()) {
    if (hyphenator->script() == script) {
      return hyphenator;
    }
  }
  return nullptr;
}

// Converts the UTF-8 word into codepoint metadata for downstream rules.
std::vector<CodepointInfo> collectCodepoints(const std::string& word) {
  std::vector<CodepointInfo> cps;
  cps.reserve(word.size());

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  while (*ptr != 0) {
    const unsigned char* current = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);
    cps.push_back({cp, static_cast<size_t>(current - base)});
  }

  return cps;
}

std::vector<size_t> collectExplicitHyphenIndexes(const std::vector<CodepointInfo>& cps) {
  std::vector<size_t> indexes;
  for (size_t i = 0; i < cps.size(); ++i) {
    if (!isExplicitHyphen(cps[i].value)) {
      continue;
    }
    if (i == 0 || i + 1 >= cps.size()) {
      continue;
    }
    if (!isAlphabetic(cps[i - 1].value) || !isAlphabetic(cps[i + 1].value)) {
      continue;
    }
    const size_t breakIndex = i + 1;
    if (breakIndex >= cps.size()) {
      continue;
    }
    if (breakIndex == 0) {
      continue;
    }
    indexes.push_back(breakIndex);
  }
  return indexes;
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

void trimTrailingFootnoteReference(std::vector<CodepointInfo>& cps) {
  if (cps.size() < 3) {
    return;
  }
  int closing = static_cast<int>(cps.size()) - 1;
  if (cps[closing].value != ']') {
    return;
  }
  int pos = closing - 1;
  if (pos < 0 || !isAsciiDigit(cps[pos].value)) {
    return;
  }
  while (pos >= 0 && isAsciiDigit(cps[pos].value)) {
    --pos;
  }
  if (pos < 0 || cps[pos].value != '[') {
    return;
  }
  if (closing - pos <= 1) {
    return;
  }
  cps.erase(cps.begin() + pos, cps.end());
}

// Rejects words containing punctuation or digits unless forced.
bool hasOnlyAlphabetic(const std::vector<CodepointInfo>& cps) {
  if (cps.empty()) {
    return false;
  }

  for (const auto& info : cps) {
    if (!isAlphabetic(info.value)) {
      return false;
    }
  }
  return true;
}

// Asks the language hyphenator for legal break positions inside the word.
std::vector<size_t> collectBreakIndexes(const std::vector<CodepointInfo>& cps) {
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return {};
  }

  const Script script = detectScript(cps);
  if (const auto* hyphenator = hyphenatorForScript(script)) {
    auto indexes = hyphenator->breakIndexes(cps);
    return indexes;
  }

  return {};
}

// Maps a codepoint index back to its byte offset inside the source word.
size_t byteOffsetForIndex(const std::vector<CodepointInfo>& cps, const size_t index) {
  if (index >= cps.size()) {
    return cps.empty() ? 0 : cps.back().byteOffset;
  }
  return cps[index].byteOffset;
}

}  // namespace

std::vector<size_t> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  if (word.empty()) {
    return {};
  }

  auto cps = collectCodepoints(word);
  trimSurroundingPunctuation(cps);
  trimTrailingFootnoteReference(cps);
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return {};
  }

  auto explicitIndexes = collectExplicitHyphenIndexes(cps);
  if (!explicitIndexes.empty()) {
    std::sort(explicitIndexes.begin(), explicitIndexes.end());
    explicitIndexes.erase(std::unique(explicitIndexes.begin(), explicitIndexes.end()), explicitIndexes.end());
    std::vector<size_t> byteOffsets;
    byteOffsets.reserve(explicitIndexes.size());
    for (const size_t idx : explicitIndexes) {
      byteOffsets.push_back(byteOffsetForIndex(cps, idx));
    }
    return byteOffsets;
  }

  std::vector<size_t> indexes = hasOnlyAlphabetic(cps) ? collectBreakIndexes(cps) : std::vector<size_t>();
  if (includeFallback) {
    for (size_t idx = MIN_PREFIX_CP; idx + MIN_SUFFIX_CP <= cps.size(); ++idx) {
      indexes.push_back(idx);
    }
  }

  if (indexes.empty()) {
    return {};
  }

  std::sort(indexes.begin(), indexes.end());
  indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());

  std::vector<size_t> byteOffsets;
  byteOffsets.reserve(indexes.size());
  for (const size_t idx : indexes) {
    byteOffsets.push_back(byteOffsetForIndex(cps, idx));
  }
  return byteOffsets;
}
