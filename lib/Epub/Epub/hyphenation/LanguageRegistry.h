#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const LanguageHyphenator* hyphenator;
  uint8_t minPrefix;
  uint8_t minSuffix;
  bool (*isLetter)(uint32_t);
  uint32_t (*toLower)(uint32_t);
};

struct ExternalHyphenationPatterns {
  SerializedHyphenationPatterns patterns;
  uint32_t identity;
};

using ExternalHyphenationLookup = bool (*)(const char* primaryTag, ExternalHyphenationPatterns& out);

void setExternalHyphenationLookup(ExternalHyphenationLookup lookup);
const LanguageEntry* findLanguageEntry(const char* primaryTag);
uint32_t getLanguagePatternIdentity(const char* primaryTag);

struct LanguageEntryView {
  const LanguageEntry* data;
  size_t size;

  const LanguageEntry* begin() const { return data; }
  const LanguageEntry* end() const { return data + size; }
};

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr").
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);

// Exposes the list of supported languages primarily for tooling/tests.
LanguageEntryView getLanguageEntries();
