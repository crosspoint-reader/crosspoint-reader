#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct CodepointInfo {
  uint32_t value;
  size_t byteOffset;
};

// Minimum number of codepoints required in prefix and suffix for hyphenation.
constexpr size_t MIN_PREFIX_CP = 2;
constexpr size_t MIN_SUFFIX_CP = 2;

uint32_t toLowerLatin(uint32_t cp);
uint32_t toLowerCyrillic(uint32_t cp);

bool isLatinLetter(uint32_t cp);
bool isLatinVowel(uint32_t cp);
bool isLatinConsonant(uint32_t cp);

bool isCyrillicLetter(uint32_t cp);
bool isCyrillicVowel(uint32_t cp);
bool isCyrillicConsonant(uint32_t cp);

bool isAlphabetic(uint32_t cp);
bool isPunctuation(uint32_t cp);
bool isAsciiDigit(uint32_t cp);
bool isExplicitHyphen(uint32_t cp);
bool isSoftHyphen(uint32_t cp);
void trimSurroundingPunctuation(std::vector<CodepointInfo>& cps);
bool hasOnlyAlphabetic(const std::vector<CodepointInfo>& cps);
