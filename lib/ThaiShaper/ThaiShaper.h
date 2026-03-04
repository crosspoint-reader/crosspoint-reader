#pragma once

/**
 * Thai Text Shaping Library
 *
 * Provides proper rendering support for Thai script including:
 * - Character classification and type detection
 * - Grapheme cluster building with proper glyph ordering
 * - Mark positioning (above vowels, below vowels, tone marks)
 * - Word segmentation for line breaking
 *
 * Usage:
 *   1. Check if text contains Thai: ThaiShaper::containsThai(text)
 *   2. Build clusters: ThaiClusterBuilder::buildClusters(text)
 *   3. Render each cluster's positioned glyphs
 *
 * For line breaking:
 *   1. Find break points: ThaiWordBreak::findBreakPoints(text)
 *   2. Or get next word boundary: ThaiWordBreak::nextWordBoundary(text, offset)
 */

#include "ThaiCharacter.h"
#include "ThaiCluster.h"
#include "ThaiClusterBuilder.h"
#include "ThaiWordBreak.h"
