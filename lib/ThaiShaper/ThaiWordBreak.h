#pragma once

#include <cstddef>

namespace ThaiShaper {

/**
 * Thai Word Break - Cluster-based segmentation
 *
 * Thai text has no spaces between words. This module provides simple
 * cluster-based segmentation for line breaking. Each Thai syllable
 * (consonant + vowels + tone marks) forms a breakable unit.
 *
 * This is a lightweight implementation suitable for embedded systems
 * with limited memory. It breaks at grapheme cluster boundaries rather
 * than true word boundaries, which provides reasonable line breaking
 * without requiring a large dictionary.
 */

/**
 * Get the byte offset of the next cluster boundary.
 *
 * Walks forward from startOffset consuming one Thai grapheme cluster
 * (leading vowel + consonant + combining marks), or one non-Thai
 * codepoint, and returns the byte offset immediately after it.
 *
 * @param text   UTF-8 encoded text (null-terminated)
 * @param startOffset  Starting byte offset
 * @return Byte offset of next boundary, or startOffset if at end / null
 */
size_t nextClusterBoundary(const char* text, size_t startOffset);

}  // namespace ThaiShaper
