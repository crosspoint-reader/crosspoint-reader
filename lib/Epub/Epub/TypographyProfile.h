#pragma once

#include <cstdint>
#include <string>

class LanguageHyphenator;

// How a language's text is laid out, chosen once per section build from the book's language tag (OPF
// dc:language). Layout asks the profile instead of testing for "a CJK book" in several places, so a language
// added or corrected is one entry in TypographyProfile.cpp.
//
// Rules that follow from the characters alone stay with the characters: CJK line breaking and kinsoku
// (CjkLineBreak.h) and the Simplified Chinese punctuation widths on CJK punctuation apply in every book. The
// profile decides what needs the language.
struct TypographyProfile {
  // Where lines may break. Informational in this round: CjkLineBreak applies its rules by character.
  enum class Breaking : uint8_t { AtSpaces, BetweenCharacters, KoreanWords };
  // What a justified line stretches. Informational in this round, as Breaking.
  enum class Justify : uint8_t { WordSpaces, BetweenCharacters };
  // The punctuation width table (CjkPunctuation.h).
  enum class Punctuation : uint8_t { SimplifiedChinese, TraditionalChinese, Japanese };
  // Which way characters run inside a line. Only LeftToRight is laid out yet; RightToLeft (Arabic, Hebrew)
  // will land in the base direction CompiledContent.h reserves.
  enum class Direction : uint8_t { LeftToRight, RightToLeft };

  Breaking breaking = Breaking::AtSpaces;
  Justify justify = Justify::WordSpaces;
  Punctuation punctuation = Punctuation::SimplifiedChinese;
  // A quarter em between Han or kana and a Latin letter or digit with no space between them (PR A3b).
  bool cjkLatinGap = false;
  // Text width and first-line indent rounded down to whole ems, so CJK characters sit in columns.
  bool wholeEmGrid = false;
  Direction direction = Direction::LeftToRight;
  // A book may set this language vertically (CSS writing-mode). Nothing reads it yet.
  bool verticalAllowed = false;
  // The language's hyphenation patterns, null when it has none.
  const LanguageHyphenator* hyphenator = nullptr;

  // The profile for a BCP 47 tag ("ja", "zh-Hant-TW", "zh_CN", "en-US"); Latin rules for an empty or unknown
  // one. Case and '-'/'_' do not matter.
  static TypographyProfile forLanguage(const std::string& tag);
};
