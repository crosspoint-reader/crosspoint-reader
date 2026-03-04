#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ThaiShaper {

/**
 * Thai Word Break - Cluster-based segmentation
 *
 * Thai text has no spaces between words. This class provides simple
 * cluster-based segmentation for line breaking. Each Thai syllable
 * (consonant + vowels + tone marks) forms a breakable unit.
 *
 * This is a lightweight implementation suitable for embedded systems
 * with limited memory. It breaks at grapheme cluster boundaries rather
 * than true word boundaries, which provides reasonable line breaking
 * without requiring a large dictionary.
 */
class ThaiWordBreak {
 public:
  /**
   * Segment Thai text into breakable clusters.
   *
   * @param text UTF-8 encoded Thai text
   * @return Vector of cluster strings
   */
  static std::vector<std::string> segmentWords(const char* text);

  /**
   * Get the byte offset of the next cluster boundary.
   *
   * @param text UTF-8 encoded text
   * @param startOffset Starting byte offset
   * @return Byte offset of next boundary, or string length if at end
   */
  static size_t nextClusterBoundary(const char* text, size_t startOffset);
};

}  // namespace ThaiShaper
