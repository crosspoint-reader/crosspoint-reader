#include "HyphenationCommon.h"

namespace {

uint32_t toLowerLatinImpl(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') {
    return cp - 'A' + 'a';
  }
  return cp;
}

uint32_t toLowerCyrillicImpl(const uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x042F) {
    return cp + 0x20;
  }
  if (cp == 0x0401) {
    return 0x0451;
  }
  return cp;
}

}  // namespace

uint32_t toLowerLatin(const uint32_t cp) { return toLowerLatinImpl(cp); }

uint32_t toLowerCyrillic(const uint32_t cp) { return toLowerCyrillicImpl(cp); }

bool isLatinLetter(const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); }

bool isLatinVowel(uint32_t cp) {
  cp = toLowerLatinImpl(cp);
  return cp == 'a' || cp == 'e' || cp == 'i' || cp == 'o' || cp == 'u' || cp == 'y';
}

bool isLatinConsonant(const uint32_t cp) { return isLatinLetter(cp) && !isLatinVowel(cp); }

bool isCyrillicLetter(const uint32_t cp) { return (cp >= 0x0400 && cp <= 0x052F); }

bool isCyrillicVowel(uint32_t cp) {
  cp = toLowerCyrillicImpl(cp);
  switch (cp) {
    case 0x0430:  // а
    case 0x0435:  // е
    case 0x0451:  // ё
    case 0x0438:  // и
    case 0x043E:  // о
    case 0x0443:  // у
    case 0x044B:  // ы
    case 0x044D:  // э
    case 0x044E:  // ю
    case 0x044F:  // я
      return true;
    default:
      return false;
  }
}

bool isCyrillicConsonant(const uint32_t cp) { return isCyrillicLetter(cp) && !isCyrillicVowel(cp); }

bool isAlphabetic(const uint32_t cp) { return isLatinLetter(cp) || isCyrillicLetter(cp) || isPunctuation(cp); }

bool isVowel(const uint32_t cp) { return isLatinVowel(cp) || isCyrillicVowel(cp); }

bool isPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case '!':
    case '?':
    case ';':
    case ':':
    case '"':
    case '\'':
    case ')':
    case '(':
    case '«':
    case '»':
    case '‘':
    case '’':
    case '“':
    case '”':
    case '—':
    case '-':
    case '–':
    case '―':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x00BB:  // »
    case 0x203A:  // ›
    case 0x2026:  // …
      return true;
    default:
      return false;
  }
}

void trimTrailingPunctuation(std::vector<CodepointInfo>& cps) {
  while (!cps.empty() && isPunctuation(cps.back().value)) {
    cps.pop_back();
  }
}

Script detectScript(const std::vector<CodepointInfo>& cps) {
  bool hasLatin = false;
  bool hasCyrillic = false;
  for (const auto& info : cps) {
    if (isLatinLetter(info.value)) {
      hasLatin = true;
    } else if (isCyrillicLetter(info.value)) {
      hasCyrillic = true;
    }
  }

  if (hasLatin && !hasCyrillic) {
    return Script::Latin;
  }
  if (!hasLatin && hasCyrillic) {
    return Script::Cyrillic;
  }
  return Script::Mixed;
}
