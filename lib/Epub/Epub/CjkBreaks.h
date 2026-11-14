#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared CJK break-opportunity rules. Used by ParsedText (line breaking of body text) and by
// TextBlock (splitting a ruby annotation into the same "characters" for ruby-align: space-around).
namespace CjkBreaks {

// True when a line break is not allowed before the CJK punctuation codepoint
bool isNoBreakBeforeCjkPunctuation(const uint32_t cp);

// True when a line break is not allowed after the CJK punctuation codepoint
bool isNoBreakAfterCjkPunctuation(const uint32_t cp);

// True when a line break is allowed between two adjacent codepoints (UAX#14-like CJK rules).
bool boundaryAllowsBreak(uint32_t leftCp, uint32_t rightCp);

// Same as boundaryAllowsBreak, but Hangul boundaries are not gap-less break opportunities
// (Korean separates words with spaces).
bool hasBreakOpportunityBetween(uint32_t leftCp, uint32_t rightCp);

// Byte offsets inside `text` where it splits into tokens. Empty => the text is a single token.
// Combining marks and no-break punctuation stay attached to their neighbour.
std::vector<size_t> characterBreakByteOffsets(const std::string& text);

}  // namespace CjkBreaks
