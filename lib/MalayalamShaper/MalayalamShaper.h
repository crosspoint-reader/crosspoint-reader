#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Lightweight Malayalam text shaper.
 *
 * Transforms Malayalam codepoint sequences into shaped glyph codepoints
 * (using PUA assignments for conjunct glyphs). Operates on UTF-8 buffers
 * with no heap allocation.
 *
 * Shaping is applied greedily using longest-match-first against the
 * lookup tables generated from Manjari's GSUB tables.
 */
class MalayalamShaper {
 public:
  /**
   * Shape a UTF-8 string containing Malayalam text.
   *
   * Scans the input for Malayalam sequences and applies GSUB-derived
   * substitution rules (conjunct formation, vowel sign composition, etc).
   * Non-Malayalam codepoints pass through unchanged.
   *
   * @param input      Input UTF-8 string
   * @param inputLen   Length of input in bytes
   * @param output     Output buffer (must be at least inputLen * 2 bytes for safety)
   * @param outputCap  Capacity of output buffer in bytes
   * @return           Number of bytes written to output
   */
  static size_t shape(const char* input, size_t inputLen, char* output, size_t outputCap);

  /**
   * Returns true if the buffer contains any Malayalam codepoints (U+0D00-U+0D7F).
   */
  static bool containsMalayalam(const char* text, size_t len);

 private:
  // Decode one UTF-8 codepoint, advance pointer. Returns 0 on end/error.
  static uint32_t nextCodepoint(const char*& p, const char* end);

  // Encode one codepoint as UTF-8, advance pointer. Returns bytes written.
  static size_t encodeCodepoint(uint32_t cp, char*& out, const char* outEnd);

  // Try to match the longest rule starting at the current position.
  // Returns output codepoint if matched, 0 if no match.
  // On match, advances 'cps' index past consumed input codepoints.
  static uint32_t tryMatch(const uint32_t* cps, size_t count, size_t& pos);
};
