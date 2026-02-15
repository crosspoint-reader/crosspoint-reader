#pragma once

#include <cstddef>
#include <string>
#include <vector>

class LanguageHyphenator;

class Hyphenator {
 public:
  struct BreakInfo {
    size_t byteOffset;
    bool requiresInsertedHyphen;
  };
  // Returns byte offsets where the word may be hyphenated. When includeFallback is true, all positions obeying the
  // minimum prefix/suffix constraints are returned even if no language-specific rule matches.
  static std::vector<BreakInfo> breakOffsets(const std::string& word, bool includeFallback);

  // Provide a publication-level language hint (e.g. "en", "en-US", "ru") used to select hyphenation rules.
  static void setPreferredLanguage(const std::string& lang);

  // Returns true if a language-specific hyphenator is currently active (language is supported).
  // Returns false if current language lacks Liang patterns, indicating fallback breaks may be needed.
  static bool hasLanguageSupport() { return cachedHyphenator_ != nullptr; }

 private:
  static const LanguageHyphenator* cachedHyphenator_;
};