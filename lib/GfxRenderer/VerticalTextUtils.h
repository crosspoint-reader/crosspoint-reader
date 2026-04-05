#pragma once

#include <cstdint>

namespace VerticalTextUtils {

// Character behavior in vertical text layout
enum class VerticalBehavior : uint8_t {
  Upright,      // CJK ideographs, kana - draw normally, advance downward
  Sideways,     // Latin letters, 3+ digit numbers - rotate 90 CW
  TateChuYoko,  // 1-2 digit numbers - horizontal-in-vertical
};

// Punctuation offset for vertical text (ratio of character size, in 1/8 units)
struct PunctuationOffset {
  uint32_t codepoint;
  int8_t dxEighths;  // horizontal offset in 1/8 of charWidth
  int8_t dyEighths;  // vertical offset in 1/8 of charHeight
  bool rotate;       // true = rotate 90 CW (e.g. long vowel mark)
};

// CJK punctuation and brackets that need 90 CW rotation in vertical text.
// Horizontal font glyphs are designed for horizontal layout; rotating them
// naturally transforms their position to the correct vertical placement
// (e.g. 。at bottom-left becomes upper-right after rotation).
// dx/dyEighths are post-rotation fine-tuning offsets (usually 0).
static constexpr PunctuationOffset VERTICAL_PUNCTUATION[] = {
    // Punctuation - rotate to reposition from horizontal to vertical placement
    {0x3001, 0, 0, true},     // 、 ideographic comma
    {0x3002, 0, 0, true},     // 。 ideographic period
    {0xFF0C, 0, 0, true},     // ， fullwidth comma
    {0xFF0E, 0, 0, true},     // ． fullwidth period
    {0xFF01, 0, 0, true},     // ！ fullwidth exclamation
    {0xFF1F, 0, 0, true},     // ？ fullwidth question mark
    {0xFF1A, 0, 0, true},     // ： fullwidth colon
    {0xFF1B, 0, 0, true},     // ； fullwidth semicolon
    // Brackets - rotate so opening/closing direction matches vertical flow
    {0x300C, 0, 0, true},     // 「 left corner bracket
    {0x300D, 0, 0, true},     // 」 right corner bracket
    {0x300E, 0, 0, true},     // 『 left white corner bracket
    {0x300F, 0, 0, true},     // 』 right white corner bracket
    {0x3010, 0, 0, true},     // 【 left black lenticular bracket
    {0x3011, 0, 0, true},     // 】 right black lenticular bracket
    {0xFF08, 0, 0, true},     // （ fullwidth left paren
    {0xFF09, 0, 0, true},     // ） fullwidth right paren
    {0x3008, 0, 0, true},     // 〈 left angle bracket
    {0x3009, 0, 0, true},     // 〉 right angle bracket
    {0x300A, 0, 0, true},     // 《 left double angle bracket
    {0x300B, 0, 0, true},     // 》 right double angle bracket
    {0x3014, 0, 0, true},     // 〔 left tortoise shell bracket
    {0x3015, 0, 0, true},     // 〕 right tortoise shell bracket
    // Long marks - rotate to vertical orientation
    {0x30FC, 0, 0, true},     // ー katakana long vowel mark
    {0x2014, 0, 0, true},     // — em dash
    {0x2015, 0, 0, true},     // ― horizontal bar
    {0x2026, 0, 0, true},     // … ellipsis
    {0xFF5E, 0, 0, true},     // ～ fullwidth tilde
};
static constexpr int VERTICAL_PUNCTUATION_COUNT =
    sizeof(VERTICAL_PUNCTUATION) / sizeof(VERTICAL_PUNCTUATION[0]);

// Look up punctuation offset. Returns nullptr if no special handling needed.
inline const PunctuationOffset* getVerticalPunctuationOffset(uint32_t cp) {
  for (int i = 0; i < VERTICAL_PUNCTUATION_COUNT; i++) {
    if (VERTICAL_PUNCTUATION[i].codepoint == cp) return &VERTICAL_PUNCTUATION[i];
  }
  return nullptr;
}

// Determine if a codepoint should be drawn upright in vertical text.
// CJK ideographs, kana, CJK symbols, fullwidth forms, etc.
inline bool isUprightInVertical(uint32_t cp) {
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;   // CJK Unified Ideographs
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;   // CJK Extension A
  if (cp >= 0x3040 && cp <= 0x309F) return true;   // Hiragana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;   // Katakana
  if (cp >= 0x3000 && cp <= 0x303F) return true;   // CJK Symbols and Punctuation
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;   // Fullwidth Forms
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;   // CJK Compatibility Ideographs
  if (cp >= 0x3200 && cp <= 0x32FF) return true;   // Enclosed CJK Letters
  if (cp >= 0x3300 && cp <= 0x33FF) return true;   // CJK Compatibility
  if (cp >= 0x3100 && cp <= 0x312F) return true;   // Bopomofo
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;   // Hangul
  return false;
}

}  // namespace VerticalTextUtils
