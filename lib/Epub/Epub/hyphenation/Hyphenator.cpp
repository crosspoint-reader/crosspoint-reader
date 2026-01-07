#include "Hyphenator.h"

#include <Utf8.h>

#include <algorithm>
#include <vector>

#include "EnglishHyphenator.h"
#include "HyphenationCommon.h"
#include "LanguageHyphenator.h"
#include "RussianHyphenator.h"

namespace {

// Maps a BCP-47 language tag to a language-specific hyphenator.
const LanguageHyphenator* hyphenatorForLanguage(const std::string& langTag) {
  if (langTag.empty()) return nullptr;

  // Extract primary subtag and normalize to lowercase (e.g., "en-US" -> "en").
  std::string primary;
  primary.reserve(langTag.size());
  for (char c : langTag) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    primary.push_back(c);
  }
  if (primary.empty()) return nullptr;

  if (primary == "en") return &EnglishHyphenator::instance();
  if (primary == "ru") return &RussianHyphenator::instance();
  return nullptr;
}

// Cached hyphenator instance for the current preferred language.
const LanguageHyphenator*& cachedHyphenator() {
  static const LanguageHyphenator* hyphenator = nullptr;
  return hyphenator;
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

// Asks the language hyphenator for legal break positions inside the word.
std::vector<size_t> collectBreakIndexes(const std::vector<CodepointInfo>& cps) {
  if (const auto* hyphenator = cachedHyphenator()) {
    return hyphenator->breakIndexes(cps);
  }
  return {};
}

// Maps a codepoint index back to its byte offset inside the source word.
size_t byteOffsetForIndex(const std::vector<CodepointInfo>& cps, const size_t index) {
  return (index < cps.size()) ? cps[index].byteOffset : (cps.empty() ? 0 : cps.back().byteOffset);
}

// Builds a vector of break information from explicit hyphen markers in the given codepoints.
std::vector<Hyphenator::BreakInfo> buildExplicitBreakInfos(const std::vector<CodepointInfo>& cps) {
  std::vector<Hyphenator::BreakInfo> breaks;

  // Scan every codepoint looking for explicit/soft hyphen markers that are surrounded by letters.
  for (size_t i = 1; i + 1 < cps.size(); ++i) {
    const uint32_t cp = cps[i].value;
    if (!isExplicitHyphen(cp) || !isAlphabetic(cps[i - 1].value) || !isAlphabetic(cps[i + 1].value)) {
      continue;
    }
    // Offset points to the next codepoint so rendering starts after the hyphen marker.
    breaks.push_back({cps[i + 1].byteOffset, isSoftHyphen(cp)});
  }

  if (breaks.empty()) {
    return breaks;
  }

  // Sort by byte offset so we can deduplicate sequential markers in-place.
  std::sort(breaks.begin(), breaks.end(), [](const Hyphenator::BreakInfo& lhs, const Hyphenator::BreakInfo& rhs) {
    return lhs.byteOffset < rhs.byteOffset;
  });

  // Deduplicate in-place: merge entries at same offset while retaining "needs hyphen" flag.
  size_t writePos = 0;
  for (size_t readPos = 1; readPos < breaks.size(); ++readPos) {
    if (breaks[readPos].byteOffset == breaks[writePos].byteOffset) {
      // Merge: explicit hyphen wins over soft hyphen at same offset.
      breaks[writePos].requiresInsertedHyphen =
          breaks[writePos].requiresInsertedHyphen || breaks[readPos].requiresInsertedHyphen;
    } else {
      breaks[++writePos] = breaks[readPos];
    }
  }
  breaks.resize(writePos + 1);

  return breaks;
}

}  // namespace

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  if (word.empty()) {
    return {};
  }

  // Convert to codepoints and normalize word boundaries.
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuation(cps);
  trimTrailingFootnoteReference(cps);
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return {};
  }

  // Explicit hyphen markers (soft or hard) take precedence over heuristic breaks.
  auto explicitBreakInfos = buildExplicitBreakInfos(cps);
  if (!explicitBreakInfos.empty()) {
    return explicitBreakInfos;
  }

  // Ask language hyphenator for legal break points.
  std::vector<size_t> indexes = hasOnlyAlphabetic(cps) ? collectBreakIndexes(cps) : std::vector<size_t>();

  // Only add fallback breaks if needed and deduplicate if both language and fallback breaks exist.
  if (includeFallback) {
    for (size_t idx = MIN_PREFIX_CP; idx + MIN_SUFFIX_CP <= cps.size(); ++idx) {
      indexes.push_back(idx);
    }
    // Only deduplicate if we have both language-specific and fallback breaks.
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
  } else if (indexes.empty()) {
    return {};
  }

  if (indexes.empty()) {
    return {};
  }

  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    breaks.push_back({byteOffsetForIndex(cps, idx), true});
  }

  return breaks;
}

void Hyphenator::setPreferredLanguage(const std::string& lang) { cachedHyphenator() = hyphenatorForLanguage(lang); }
