#include "ThaiCharacter.h"

#include <Utf8.h>

namespace ThaiShaper {

ThaiCharType getThaiCharType(uint32_t cp) {
  // Not in Thai block
  if (cp < 0x0E00 || cp > 0x0E7F) {
    return ThaiCharType::NON_THAI;
  }

  if (cp >= 0x0E01 && cp <= 0x0E2E) {
    return ThaiCharType::CONSONANT;
  }

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

static constexpr uint32_t SARA_AM = 0x0E33;
static constexpr uint32_t SARA_AA = 0x0E32;
static constexpr uint32_t NIKHAHIT = 0x0E4D;

static void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void decomposeSaraAm(std::string& text) {
  // Sara Am is U+0E33 → UTF-8: 0xE0 0xB8 0xB3.  Fast byte scan avoids
  // any allocation or UTF-8 decoding when the string has no Sara Am.
  if (text.find("\xE0\xB8\xB3") == std::string::npos) return;

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text.data());
  const uint8_t* end = ptr + text.size();

  std::string result;
  result.reserve(text.size() + 6);

  const uint8_t* cur = ptr;
  uint32_t prevCp = 0;
  const uint8_t* prevStart = ptr;

  while (cur < end) {
    const uint8_t* cpStart = cur;
    uint32_t cp = utf8NextCodepoint(&cur);
    if (cp == 0) break;

    if (cp == SARA_AM && prevCp != 0 && (isThaiToneMark(prevCp) || prevCp == NIKHAHIT)) {
      // Remove the previously appended tone mark / Nikhahit
      size_t prevLen = static_cast<size_t>(cpStart - prevStart);
      result.resize(result.size() - prevLen);
      appendUtf8(result, NIKHAHIT);
      appendUtf8(result, prevCp);
      appendUtf8(result, SARA_AA);
      prevCp = SARA_AA;
      prevStart = cpStart;
    } else {
      result.append(reinterpret_cast<const char*>(cpStart), static_cast<size_t>(cur - cpStart));
      prevCp = cp;
      prevStart = cpStart;
    }
  }

  text = std::move(result);
}

}  // namespace ThaiShaper
