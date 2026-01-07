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

// Preferred language hint; empty means "auto".
std::string& preferredLanguage() {
  static std::string lang;
  return lang;
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
  if (cps.size() < MIN_PREFIX_CP + MIN_SUFFIX_CP) {
    return {};
  }

  // Use cached hyphenator to avoid repeated language lookups.
  if (const auto* hyphenator = cachedHyphenator()) {
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

// Builds a vector of break information from explicit hyphen markers in the given codepoints.
std::vector<Hyphenator::BreakInfo> buildExplicitBreakInfos(const std::vector<CodepointInfo>& cps) {
  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(cps.size());

  // Scan every codepoint looking for explicit/soft hyphen markers that are surrounded by letters.
  for (size_t i = 0; i < cps.size(); ++i) {
    const uint32_t cp = cps[i].value;
    if (!isExplicitHyphen(cp) || i == 0 || i + 1 >= cps.size()) {
      continue;  // Need at least one alphabetic character on both sides.
    }
    if (!isAlphabetic(cps[i - 1].value) || !isAlphabetic(cps[i + 1].value)) {
      continue;
    }
    // Offset points to the next codepoint so rendering starts after the hyphen marker.
    breaks.push_back({byteOffsetForIndex(cps, i + 1), isSoftHyphen(cp)});
  }

  if (breaks.empty()) {
    return breaks;
  }

  // Sort by byte offset so we can deduplicate sequential markers.
  // Multiple dash codepoints can point to the same byte offset once punctuation is trimmed; sort before merging.
  std::sort(breaks.begin(), breaks.end(), [](const Hyphenator::BreakInfo& lhs, const Hyphenator::BreakInfo& rhs) {
    return lhs.byteOffset < rhs.byteOffset;
  });

  // Ensure we keep a single entry per break while retaining the "needs hyphen" flag when any marker requested it.
  std::vector<Hyphenator::BreakInfo> deduped;
  deduped.reserve(breaks.size());
  for (const auto& entry : breaks) {
    if (!deduped.empty() && deduped.back().byteOffset == entry.byteOffset) {
      // Merge entries so that an explicit hyphen wins over a soft hyphen at the same offset.
      deduped.back().requiresInsertedHyphen = deduped.back().requiresInsertedHyphen || entry.requiresInsertedHyphen;
    } else {
      deduped.push_back(entry);
    }
  }

  return deduped;
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

  // Ask language hyphenator for legal break points, optionally augment with naive fallback.
  std::vector<size_t> indexes = hasOnlyAlphabetic(cps) ? collectBreakIndexes(cps) : std::vector<size_t>();
  if (includeFallback) {
    for (size_t idx = MIN_PREFIX_CP; idx + MIN_SUFFIX_CP <= cps.size(); ++idx) {
      indexes.push_back(idx);
    }
  }

  if (indexes.empty()) {
    return {};
  }

  // Sort/deduplicate break indexes before converting them back to byte offsets.
  std::sort(indexes.begin(), indexes.end());
  indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());

  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    breaks.push_back({byteOffsetForIndex(cps, idx), true});
  }

  return breaks;
}

void Hyphenator::setPreferredLanguage(const std::string& lang) {
  preferredLanguage() = lang;
  cachedHyphenator() = hyphenatorForLanguage(lang);
}
