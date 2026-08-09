#pragma once

#include <cctype>
#include <string>

// Pure headword-matching helpers for Dictionary, kept free of HAL/Arduino
// dependencies so they stay host-testable (see test/dictionary_match).
namespace DictionaryMatch {

/**
 * Turkish dotted/dotless I fold: 'I' pairs with ı (U+0131) and İ (U+0130)
 * pairs with 'i' in Turkish, so the generic ASCII mapping ('I' → 'i', 'İ'
 * untouched) folds both the wrong way for Turkish headwords.
 *
 * Returns the word with 'I' → "ı" and "İ" → "i" applied, or an empty string
 * when the word contains neither (no second reading worth probing). Must run
 * on the raw word: after Dictionary::cleanWord's ASCII tolower the original
 * 'I' is indistinguishable from 'i'.
 */
inline std::string turkishIFold(const char* word) {
  if (!word) return "";
  const auto* b = reinterpret_cast<const unsigned char*>(word);
  std::string out;
  bool changed = false;
  for (size_t i = 0; b[i]; i++) {
    if (b[i] == 'I') {
      out += "\xC4\xB1";  // ı U+0131
      changed = true;
    } else if (b[i] == 0xC4 && b[i + 1] == 0xB0) {  // İ U+0130
      out += 'i';
      i++;
      changed = true;
    } else {
      out += static_cast<char>(b[i]);
    }
  }
  return changed ? out : std::string();
}

/**
 * True when `head` is a proper prefix of `target`, comparing byte-wise with
 * ASCII case folding — the same equivalence StringUtils::asciiCaseCmp gives
 * the index sort, so a candidate accepted here is one the sorted scan really
 * passed on the way to the target's insertion point.
 *
 * An empty `head` counts as a prefix of any non-empty target; callers enforce
 * their own minimum stem length (see Dictionary::MIN_PREFIX_STEM_CHARS).
 */
/**
 * Number of UTF-8 codepoints in `s`, counted as non-continuation bytes, so
 * length thresholds mean letters rather than bytes ("ış" is two letters even
 * though it is four bytes). Malformed input degrades gracefully: every
 * lead-like byte counts once.
 */
inline size_t utf8Length(const char* s) {
  size_t count = 0;
  for (const auto* b = reinterpret_cast<const unsigned char*>(s); *b; b++) {
    if ((*b & 0xC0) != 0x80) count++;
  }
  return count;
}

inline bool isAsciiCasePrefix(const char* head, const char* target) {
  size_t i = 0;
  for (; head[i]; i++) {
    const int a = std::tolower(static_cast<unsigned char>(head[i]));
    const int b = std::tolower(static_cast<unsigned char>(target[i]));
    if (a != b) return false;
  }
  return target[i] != '\0';  // proper prefix: the target must keep going
}

}  // namespace DictionaryMatch
