#pragma once

#include <cstddef>
#include <cstdint>

// The Indic scripts CrossPoint shapes (Brahmic scripts written with spaces
// between words): what the shaper, line breaking, font probes and the
// unshaped fallback need to know about each one.
//
// Mirrored by SCRIPTS in lib/EpdFont/scripts/shaping_blob.py, which builds the
// layout tables .cpfont files carry for these scripts.
namespace indic {

struct ScriptInfo {
  const char* name;
  uint32_t first;   // first codepoint of the script's 128-codepoint Unicode block
  uint32_t isoTag;  // ISO 15924 code packed big-endian, as hb_script_from_iso15924_tag() takes it
  uint32_t probe;   // letter KA: a font that maps it draws the script

  constexpr uint32_t last() const { return first + 0x7F; }
};

constexpr uint32_t isoTag(const char (&code)[5]) {
  return (static_cast<uint32_t>(code[0]) << 24) | (static_cast<uint32_t>(code[1]) << 16) |
         (static_cast<uint32_t>(code[2]) << 8) | static_cast<uint32_t>(code[3]);
}

// Consecutive Unicode blocks from U+0900, so scriptOf() is an index.
inline constexpr ScriptInfo SCRIPTS[] = {
    {"Devanagari", 0x0900, isoTag("Deva"), 0x0915},  // Hindi, Marathi, Nepali, Sanskrit
    {"Bengali", 0x0980, isoTag("Beng"), 0x0995},     // Bengali, Assamese
    {"Gurmukhi", 0x0A00, isoTag("Guru"), 0x0A15},    // Punjabi
    {"Gujarati", 0x0A80, isoTag("Gujr"), 0x0A95},    // Gujarati
    {"Oriya", 0x0B00, isoTag("Orya"), 0x0B15},       // Odia
    {"Tamil", 0x0B80, isoTag("Taml"), 0x0B95},       // Tamil
    {"Telugu", 0x0C00, isoTag("Telu"), 0x0C15},      // Telugu
    {"Kannada", 0x0C80, isoTag("Knda"), 0x0C95},     // Kannada
    {"Malayalam", 0x0D00, isoTag("Mlym"), 0x0D15},   // Malayalam
    {"Sinhala", 0x0D80, isoTag("Sinh"), 0x0D9A},     // Sinhala
};
inline constexpr size_t SCRIPT_COUNT = sizeof(SCRIPTS) / sizeof(SCRIPTS[0]);
inline constexpr uint32_t FIRST_CODEPOINT = 0x0900;
inline constexpr uint32_t LAST_CODEPOINT = 0x0DFF;

constexpr bool scriptsAreConsecutiveBlocks() {
  for (size_t i = 0; i < SCRIPT_COUNT; i++) {
    if (SCRIPTS[i].first != FIRST_CODEPOINT + 0x80 * i) return false;
  }
  return SCRIPTS[SCRIPT_COUNT - 1].last() == LAST_CODEPOINT;
}
static_assert(scriptsAreConsecutiveBlocks(), "scriptOf() indexes SCRIPTS by block");

constexpr uint32_t ZWNJ = 0x200C;
constexpr uint32_t ZWJ = 0x200D;
constexpr uint32_t DOTTED_CIRCLE = 0x25CC;

// Codepoints every Indic script uses, which belong to the script of the text
// around them: the Vedic stress signs and dandas (encoded in the Devanagari
// block), the joiners that steer conjunct formation, and the dotted circle
// that stands in for a missing base.
constexpr bool isShared(const uint32_t cp) {
  return (cp >= 0x0951 && cp <= 0x0954) || cp == 0x0964 || cp == 0x0965 || cp == ZWNJ || cp == ZWJ ||
         cp == DOTTED_CIRCLE;
}

// The script `cp` is written in, or nullptr for shared and non-Indic codepoints.
constexpr const ScriptInfo* scriptOf(const uint32_t cp) {
  if (cp < FIRST_CODEPOINT || cp > LAST_CODEPOINT || isShared(cp)) return nullptr;
  return &SCRIPTS[(cp - FIRST_CODEPOINT) >> 7];
}

constexpr size_t indexOf(const ScriptInfo& script) { return static_cast<size_t>(&script - SCRIPTS); }

// True when `utf8` contains any codepoint of the Indic blocks. A byte scan:
// U+0900-U+0DFF encode as E0 A4 xx .. E0 B7 xx.
inline bool containsIndic(const char* utf8) {
  if (utf8 == nullptr) return false;
  for (const auto* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p) {
    if (p[0] == 0xE0 && p[1] >= 0xA4 && p[1] <= 0xB7) return true;
  }
  return false;
}

// Viramas (Unicode canonical combining class 9): they kill a consonant's
// inherent vowel and, in most scripts, bind the next consonant into a conjunct.
inline constexpr uint32_t VIRAMAS[] = {0x094D, 0x09CD, 0x0A4D, 0x0ACD, 0x0B4D, 0x0BCD,
                                       0x0C4D, 0x0CCD, 0x0D3B, 0x0D3C, 0x0D4D, 0x0DCA};
// Nuktas (combining class 7): a dot below that makes a new consonant.
inline constexpr uint32_t NUKTAS[] = {0x093C, 0x09BC, 0x0A3C, 0x0ABC, 0x0B3C, 0x0C3C, 0x0CBC};

constexpr bool isVirama(const uint32_t cp) {
  for (const uint32_t v : VIRAMAS) {
    if (v == cp) return true;
  }
  return false;
}

constexpr bool isNukta(const uint32_t cp) {
  for (const uint32_t n : NUKTAS) {
    if (n == cp) return true;
  }
  return false;
}

struct CodepointRange {
  uint32_t first;
  uint32_t last;
};

// Every combining sign of the Indic blocks: Unicode 15 general categories Mn
// and Mc in U+0900-U+0DFF (vowel signs, nuktas, viramas, candrabindu,
// anusvara, visarga, stress and length marks). Sorted.
inline constexpr CodepointRange DEPENDENT_SIGNS[] = {
    // Devanagari
    {0x0900, 0x0903},
    {0x093A, 0x093C},
    {0x093E, 0x094F},
    {0x0951, 0x0957},
    {0x0962, 0x0963},
    // Bengali
    {0x0981, 0x0983},
    {0x09BC, 0x09BC},
    {0x09BE, 0x09C4},
    {0x09C7, 0x09C8},
    {0x09CB, 0x09CD},
    {0x09D7, 0x09D7},
    {0x09E2, 0x09E3},
    {0x09FE, 0x09FE},
    // Gurmukhi
    {0x0A01, 0x0A03},
    {0x0A3C, 0x0A3C},
    {0x0A3E, 0x0A42},
    {0x0A47, 0x0A48},
    {0x0A4B, 0x0A4D},
    {0x0A51, 0x0A51},
    {0x0A70, 0x0A71},
    {0x0A75, 0x0A75},
    // Gujarati
    {0x0A81, 0x0A83},
    {0x0ABC, 0x0ABC},
    {0x0ABE, 0x0AC5},
    {0x0AC7, 0x0AC9},
    {0x0ACB, 0x0ACD},
    {0x0AE2, 0x0AE3},
    {0x0AFA, 0x0AFF},
    // Oriya
    {0x0B01, 0x0B03},
    {0x0B3C, 0x0B3C},
    {0x0B3E, 0x0B44},
    {0x0B47, 0x0B48},
    {0x0B4B, 0x0B4D},
    {0x0B55, 0x0B57},
    {0x0B62, 0x0B63},
    // Tamil
    {0x0B82, 0x0B82},
    {0x0BBE, 0x0BC2},
    {0x0BC6, 0x0BC8},
    {0x0BCA, 0x0BCD},
    {0x0BD7, 0x0BD7},
    // Telugu
    {0x0C00, 0x0C04},
    {0x0C3C, 0x0C3C},
    {0x0C3E, 0x0C44},
    {0x0C46, 0x0C48},
    {0x0C4A, 0x0C4D},
    {0x0C55, 0x0C56},
    {0x0C62, 0x0C63},
    // Kannada
    {0x0C81, 0x0C83},
    {0x0CBC, 0x0CBC},
    {0x0CBE, 0x0CC4},
    {0x0CC6, 0x0CC8},
    {0x0CCA, 0x0CCD},
    {0x0CD5, 0x0CD6},
    {0x0CE2, 0x0CE3},
    {0x0CF3, 0x0CF3},
    // Malayalam
    {0x0D00, 0x0D03},
    {0x0D3B, 0x0D3C},
    {0x0D3E, 0x0D44},
    {0x0D46, 0x0D48},
    {0x0D4A, 0x0D4D},
    {0x0D57, 0x0D57},
    {0x0D62, 0x0D63},
    // Sinhala
    {0x0D81, 0x0D83},
    {0x0DCA, 0x0DCA},
    {0x0DCF, 0x0DD4},
    {0x0DD6, 0x0DD6},
    {0x0DD8, 0x0DDF},
    {0x0DF2, 0x0DF3},
};

constexpr bool isDependentSign(const uint32_t cp) {
  if (cp < FIRST_CODEPOINT || cp > LAST_CODEPOINT) return false;
  size_t lo = 0;
  size_t hi = sizeof(DEPENDENT_SIGNS) / sizeof(DEPENDENT_SIGNS[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (cp < DEPENDENT_SIGNS[mid].first) {
      hi = mid;
    } else if (cp > DEPENDENT_SIGNS[mid].last) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

// False when a line break between `prev` and `cur` would split a syllable:
// before a sign or joiner, or after a virama or ZWJ (which bind the next
// consonant into a conjunct). Each half of a split syllable would shape on
// its own. Everything else may break.
constexpr bool syllableBreakAllowed(const uint32_t prev, const uint32_t cur) {
  const bool curBindsBack = isDependentSign(cur) || cur == ZWNJ || cur == ZWJ;
  return !curBindsBack && !isVirama(prev) && prev != ZWJ;
}

}  // namespace indic
