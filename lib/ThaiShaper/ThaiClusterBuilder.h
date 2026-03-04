#pragma once

#include <cstdint>
#include <vector>

#include "ThaiCharacter.h"
#include "ThaiCluster.h"

namespace ThaiShaper {

/**
 * Thai Cluster Builder
 *
 * Takes a UTF-8 string containing Thai text and builds a sequence of
 * grapheme clusters with proper glyph positioning for rendering.
 *
 * Thai text shaping involves:
 * 1. Reordering: Leading vowels (เ แ โ ไ ใ) appear AFTER the consonant in
 *    Unicode but must be rendered BEFORE the consonant visually.
 *
 * 2. Stacking: Above vowels, tone marks, and other marks must be stacked
 *    vertically above the base consonant.
 *
 * 3. Below placement: Below vowels must be positioned under the consonant.
 *
 * Example: "เกี่ยว" (about/related) is stored as:
 *   เ (U+0E40) + ก (U+0E01) + ี (U+0E35) + ่ (U+0E48) + ย (U+0E22) + ว (U+0E27)
 *
 * But renders as:
 *   [เ][ก with ี and ่ stacked above][ย][ว]
 *
 * Note: Consonant "ascenders" - Some Thai consonants have tall ascenders
 * (ป ฝ ฟ etc.) that may cause above marks to shift position. This
 * implementation uses a simplified approach suitable for e-ink rendering.
 */
class ThaiClusterBuilder {
 public:
  /**
   * Build clusters from a UTF-8 Thai text string.
   *
   * @param text UTF-8 encoded string (may contain mixed Thai and non-Thai)
   * @return Vector of clusters, each representing one grapheme unit
   */
  static std::vector<ThaiCluster> buildClusters(const char* text);

  /**
   * Build a single cluster from codepoints starting at the current position.
   * Advances the text pointer past the consumed codepoints.
   *
   * @param text Pointer to pointer of UTF-8 text (advanced on return)
   * @return Single cluster containing positioned glyphs
   */
  static ThaiCluster buildNextCluster(const uint8_t** text);

 private:
  // Check if a consonant is a "tall" consonant that affects mark positioning
  static bool isAscenderConsonant(uint32_t cp);

  // Check if a consonant is a "descender" consonant that affects below-mark positioning
  static bool isDescenderConsonant(uint32_t cp);
};

}  // namespace ThaiShaper
