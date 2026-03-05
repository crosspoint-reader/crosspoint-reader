#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ThaiShaper {

/**
 * Thai Word Break - Cluster-based segmentation
 *
 * Thai text has no spaces between words. This function provides simple
 * cluster-based segmentation for line breaking. Each Thai syllable
 * (consonant + vowels + tone marks) forms a breakable unit.
 *
 * This is a lightweight implementation suitable for embedded systems
 * with limited memory. It breaks at grapheme cluster boundaries rather
 * than true word boundaries, which provides reasonable line breaking
 * without requiring a large dictionary.
 */

/**
* Segment Thai text into breakable clusters.
*
* @param text UTF-8 encoded Thai text
* @return Vector of cluster strings
*/
std::vector<std::string> segmentWords(const char* text);

}  // namespace ThaiShaper
