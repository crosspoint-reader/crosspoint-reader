#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyphenation/Hyphenator.h"

// Where Chinese, Japanese and Korean text may break between characters, and where kinsoku (the CJK
// line-edge rules) forbids it.
//
// Ported from crosspoint-reader (lib/Epub/Epub/ParsedText.cpp at 83f9cc05): the boundary rules,
// breakByteOffsets and the punctuation sets are from PR #2288 ("feat: Support for Korean line breaks
// and glyph spacing", Justin Mitchell), the dashes in the no-line-start set from PR #3883 (Phạm Bình
// An), the Hangul exclusion and hangulLineEndBreaks from PR #3700 (Sung-jin Brian Hong). Ours: the
// characters added to both sets from KOReader's crengine (UAX #14 classes CL, EX, NS, IN and OP;
// spec Appendix A.1), the straight quotes (QU, LB19), the ideographic space (BA) and the middle
// dot, and this module.
namespace CjkLineBreak {

// Kinsoku: characters that must not start a line (closing brackets and quotes, sentence and
// clause punctuation, iteration marks, dashes and ellipses), and characters that must not end one
// (opening brackets and quotes). A straight quote (" ' ＂ ＇) may open or close, so it is in both.
// Small kana and the prolonged sound mark may start a line, as in KOReader's default.
bool isNoLineStart(uint32_t cp);
bool isNoLineEnd(uint32_t cp);

// A break between these two codepoints is allowed by the CJK rules: one side is CJK (or the left
// side is a visible dash), and kinsoku does not forbid it.
bool boundaryAllowsBreak(uint32_t leftCp, uint32_t rightCp);

// A boundary that becomes a breakable zero-width gap: boundaryAllowsBreak, and neither side is
// Hangul. Korean separates words with spaces, so Hangul words stay whole.
bool isGapBetween(uint32_t leftCp, uint32_t rightCp);

// The byte offsets inside `text` where it splits into CJK tokens: after each codepoint followed by
// a gap (isGapBetween). Empty for text without a CJK codepoint or with a single codepoint, so Latin
// words are never split here. Kinsoku pairs such as 字。 or 「漢 produce no offset.
std::vector<size_t> breakByteOffsets(const std::string& text);

// Line-end split points inside a Hangul word: between syllables, and where Hangul meets digits,
// Latin or brackets, never before closing punctuation. No hyphen is drawn at any of them.
std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word);

}  // namespace CjkLineBreak
