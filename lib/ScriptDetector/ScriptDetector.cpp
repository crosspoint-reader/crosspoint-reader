#include "ScriptDetector.h"

#include <Utf8.h>

#include <cctype>

namespace ScriptDetector {

namespace {

bool startsWithLanguage(const char* languageTag, const char* prefix) {
  if (languageTag == nullptr || prefix == nullptr) {
    return false;
  }

  size_t index = 0;
  while (prefix[index] != '\0') {
    const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(languageTag[index])));
    const char rhs = prefix[index];
    if (lhs != rhs) {
      return false;
    }
    ++index;
  }

  const char next = static_cast<char>(std::tolower(static_cast<unsigned char>(languageTag[index])));
  return next == '\0' || next == '-' || next == '_';
}

}  // namespace

bool isArabicCodepoint(const uint32_t codepoint) {
  return (codepoint >= 0x0600 && codepoint <= 0x06FF) || (codepoint >= 0x0750 && codepoint <= 0x077F) ||
         (codepoint >= 0x08A0 && codepoint <= 0x08FF) || (codepoint >= 0xFB50 && codepoint <= 0xFDFF) ||
         (codepoint >= 0xFE70 && codepoint <= 0xFEFF) || (codepoint >= 0x1EE00 && codepoint <= 0x1EEFF);
}

bool containsArabic(const char* text) {
  if (text == nullptr) {
    return false;
  }

  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != 0) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (isArabicCodepoint(codepoint)) {
      return true;
    }
  }

  return false;
}

bool isRtlLanguageTag(const char* languageTag) {
  return startsWithLanguage(languageTag, "ar") || startsWithLanguage(languageTag, "fa") ||
         startsWithLanguage(languageTag, "ur") || startsWithLanguage(languageTag, "ps") ||
         startsWithLanguage(languageTag, "sd") || startsWithLanguage(languageTag, "ug");
}

}  // namespace ScriptDetector
