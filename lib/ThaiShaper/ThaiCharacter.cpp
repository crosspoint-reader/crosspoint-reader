#include "ThaiCharacter.h"

#include <Utf8.h>

namespace ThaiShaper {

ThaiCharType getThaiCharType(uint32_t cp) {
  // Not in Thai block
  if (cp < 0x0E00 || cp > 0x0E7F) {
    return ThaiCharType::NON_THAI;
  }

  // Thai consonants: ก-ฮ (U+0E01-U+0E2E)
  // Note: U+0E2F (ฯ) is PAIYANNOI, a punctuation mark
  if (cp >= 0x0E01 && cp <= 0x0E2E) {
    return ThaiCharType::CONSONANT;
  }

  // Leading vowels: เ แ โ ไ ใ (U+0E40-U+0E44)
  if (cp >= 0x0E40 && cp <= 0x0E44) {
    return ThaiCharType::LEADING_VOWEL;
  }

  // Above vowels and marks
  switch (cp) {
    case 0x0E31:  // MAI HAN-AKAT (ั)
    case 0x0E34:  // SARA I (ิ)
    case 0x0E35:  // SARA II (ี)
    case 0x0E36:  // SARA UE (ึ)
    case 0x0E37:  // SARA UEE (ื)
    case 0x0E47:  // MAITAIKHU (็)
      return ThaiCharType::ABOVE_VOWEL;
  }

  // Below vowels
  switch (cp) {
    case 0x0E38:  // SARA U (ุ)
    case 0x0E39:  // SARA UU (ู)
    case 0x0E3A:  // PHINTHU (ฺ)
      return ThaiCharType::BELOW_VOWEL;
  }

  // Tone marks
  switch (cp) {
    case 0x0E48:  // MAI EK (่)
    case 0x0E49:  // MAI THO (้)
    case 0x0E4A:  // MAI TRI (๊)
    case 0x0E4B:  // MAI CHATTAWA (๋)
      return ThaiCharType::TONE_MARK;
  }

  // Follow vowels (vowels that display after consonant)
  switch (cp) {
    case 0x0E30:  // SARA A (ะ)
    case 0x0E32:  // SARA AA (า)
    case 0x0E33:  // SARA AM (ำ)
    case 0x0E45:  // LAKKHANGYAO (ๅ)
      return ThaiCharType::FOLLOW_VOWEL;
  }

  // Nikhahit
  if (cp == 0x0E4D) {
    return ThaiCharType::NIKHAHIT;
  }

  // Yamakkan / Thanthakhat
  if (cp == 0x0E4C || cp == 0x0E4E) {
    return ThaiCharType::YAMAKKAN;
  }

  // Thai digits: ๐-๙ (U+0E50-U+0E59)
  if (cp >= 0x0E50 && cp <= 0x0E59) {
    return ThaiCharType::THAI_DIGIT;
  }

  // Everything else in Thai block is a symbol/punctuation
  return ThaiCharType::THAI_SYMBOL;
}

bool containsThai(const char* text) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;

  while ((cp = utf8NextCodepoint(&ptr))) {
    if (isThaiCodepoint(cp)) {
      return true;
    }
  }

  return false;
}

}  // namespace ThaiShaper
