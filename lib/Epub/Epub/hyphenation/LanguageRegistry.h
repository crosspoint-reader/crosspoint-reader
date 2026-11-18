#pragma once

#include <string>

#include "LanguageHyphenator.h"

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const LanguageHyphenator* hyphenator;
};

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr").
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);
