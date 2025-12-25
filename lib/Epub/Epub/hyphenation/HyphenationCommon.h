#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct CodepointInfo {
  uint32_t value;
  size_t byteOffset;
};

enum class Script { Latin, Cyrillic, Mixed };

constexpr size_t MIN_PREFIX_CP = 3;
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
bool isVowel(uint32_t cp);
bool isPunctuation(uint32_t cp);
void trimTrailingPunctuation(std::vector<CodepointInfo>& cps);

Script detectScript(const std::vector<CodepointInfo>& cps);
