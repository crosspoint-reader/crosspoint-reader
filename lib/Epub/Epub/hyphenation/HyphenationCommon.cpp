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

bool isAlphabetic(const uint32_t cp) { return isLatinLetter(cp) || isCyrillicLetter(cp); }

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
    case 0x00AB:  // «
    case 0x00BB:  // »
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x201C:  // “
    case 0x201D:  // ”
    case '{':
    case '}':
    case '/':
    case 0x203A:  // ›
    case 0x2026:  // …
      return true;
    default:
      return false;
  }
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isExplicitHyphen(const uint32_t cp) {
  switch (cp) {
    case '-':
    case 0x00AD:  // soft hyphen
    case 0x058A:  // Armenian hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2012:  // figure dash
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:  // horizontal bar
    case 0x2043:  // hyphen bullet
    case 0x207B:  // superscript minus
    case 0x208B:  // subscript minus
    case 0x2212:  // minus sign
    case 0x2E17:  // double oblique hyphen
    case 0x2E3A:  // two-em dash
    case 0x2E3B:  // three-em dash
    case 0xFE58:  // small em dash
    case 0xFE63:  // small hyphen-minus
    case 0xFF0D:  // fullwidth hyphen-minus
      return true;
    default:
      return false;
  }
}

bool isSoftHyphen(const uint32_t cp) { return cp == 0x00AD; }

void trimSurroundingPunctuation(std::vector<CodepointInfo>& cps) {
  while (!cps.empty() && isPunctuation(cps.front().value)) {
    cps.erase(cps.begin());
  }
  while (!cps.empty() && isPunctuation(cps.back().value)) {
    cps.pop_back();
  }
}

bool hasOnlyAlphabetic(const std::vector<CodepointInfo>& cps) {
  if (cps.empty()) {
    return false;
  }

  for (const auto& info : cps) {
    if (!isAlphabetic(info.value)) {
      return false;
    }
  }
  return true;
}
