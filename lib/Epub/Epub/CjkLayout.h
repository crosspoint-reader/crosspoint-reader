#pragma once
#include <cstddef>
#include <cstdint>

// The 12 CJK punctuation marks that should NEVER lead a new line (避头标点).
inline constexpr uint32_t CJK_LEADING_PUNCTUATION[] = {
    0x3002, 0xFF0C, 0xFF01, 0xFF1F, 0xFF1B, 0xFF1A,
    0x3001, 0xFF09, 0x301B, 0x300B, 0x201D, 0x2019
};

inline constexpr size_t CJK_LEADING_PUNCTUATION_COUNT =
    sizeof(CJK_LEADING_PUNCTUATION) / sizeof(CJK_LEADING_PUNCTUATION[0]);

bool isCJKLeadingPunctuation(uint32_t cp);
