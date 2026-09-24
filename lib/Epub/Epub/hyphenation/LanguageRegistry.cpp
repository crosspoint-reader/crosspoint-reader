#include "LanguageRegistry.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-fi.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pt.trie.h"
#ifdef HYPHENATION_BENCH_EMBEDDED
#include "generated/hyph-de.trie.h"
#include "generated/hyph-hu.trie.h"
#endif

namespace {

LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator finnishHyphenator(fi_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(pt_patterns, isLatinLetter, toLowerLatin);
#ifdef HYPHENATION_BENCH_EMBEDDED
LanguageHyphenator benchmarkGerman(de_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator benchmarkHungarian(hu_patterns, isLatinLetter, toLowerLatin);
#endif

using EntryArray = std::array<LanguageEntry, 28>;
const EntryArray& entries() {
  static const EntryArray kEntries = {{
      {"afrikaans", "af", nullptr, 1, 2, isLatinLetter, toLowerLatin},
      {"albanian", "sq", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"catalan", "ca", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"croatian", "hr", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"czech", "cs", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"danish", "da", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"dutch", "nl", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"english", "en", &englishHyphenator, 3, 3, isLatinLetter, toLowerLatin},
      {"estonian", "et", nullptr, 2, 3, isLatinLetter, toLowerLatin},
      {"finnish", "fi", &finnishHyphenator, 2, 2, isLatinLetter, toLowerLatin},
      {"french", "fr", &frenchHyphenator, 2, 2, isLatinLetter, toLowerLatin},
      {"galician", "gl", nullptr, 2, 2, isLatinLetter, toLowerLatin},
#ifdef HYPHENATION_BENCH_EMBEDDED
      {"german", "de", &benchmarkGerman, 2, 2, isLatinLetter, toLowerLatin},
      {"hungarian", "hu", &benchmarkHungarian, 2, 2, isLatinLetter, toLowerLatin},
#else
      {"german", "de", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"hungarian", "hu", nullptr, 2, 2, isLatinLetter, toLowerLatin},
#endif
      {"icelandic", "is", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"italian", "it", &italianHyphenator, 2, 2, isLatinLetter, toLowerLatin},
      {"kurmanji", "ku", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"latin", "la", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"lithuanian", "lt", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"norwegian", "no", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"polish", "pl", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"portuguese", "pt", &portugueseHyphenator, 2, 2, isLatinLetter, toLowerLatin},
      {"russian", "ru", nullptr, 2, 2, isCyrillicLetter, toLowerCyrillic},
      {"slovak", "sk", nullptr, 2, 3, isLatinLetter, toLowerLatin},
      {"slovenian", "sl", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"spanish", "es", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"swedish", "sv", nullptr, 2, 2, isLatinLetter, toLowerLatin},
      {"ukrainian", "uk", nullptr, 2, 2, isCyrillicLetter, toLowerCyrillic},
  }};
  return kEntries;
}

ExternalHyphenationLookup externalLookup = nullptr;
SerializedHyphenationPatterns externalPatterns{0, nullptr, 0};
LanguageHyphenator externalHyphenator(externalPatterns, isLatinLetter, toLowerLatin);

}  // namespace

void setExternalHyphenationLookup(const ExternalHyphenationLookup lookup) { externalLookup = lookup; }

const LanguageEntry* findLanguageEntry(const char* primaryTag) {
  if (!primaryTag) return nullptr;
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(), [primaryTag](const LanguageEntry& entry) {
    return std::strcmp(primaryTag, entry.primaryTag) == 0;
  });
  return it != allEntries.end() ? &*it : nullptr;
}

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto* entry = findLanguageEntry(primaryTag.c_str());
  if (!entry) return nullptr;
  if (entry->hyphenator) return entry->hyphenator;
  if (!externalLookup) return nullptr;

  ExternalHyphenationPatterns found{};
  if (!externalLookup(entry->primaryTag, found)) return nullptr;
  externalPatterns = found.patterns;
  externalHyphenator.configure(entry->isLetter, entry->toLower, entry->minPrefix, entry->minSuffix);
  return &externalHyphenator;
}

uint32_t getLanguagePatternIdentity(const char* primaryTag) {
  const auto* entry = findLanguageEntry(primaryTag);
  if (!entry || entry->hyphenator || !externalLookup) return 0;
  ExternalHyphenationPatterns found{};
  return externalLookup(entry->primaryTag, found) ? found.identity : 0;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
