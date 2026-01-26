#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "generated/hyph-de.trie.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-ru.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_us_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(ru_ru_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 5>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", &frenchHyphenator},
                                       {"german", "de", &germanHyphenator},
                                       {"russian", "ru", &russianHyphenator},
                                       {"spanish", "es", &spanishHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
