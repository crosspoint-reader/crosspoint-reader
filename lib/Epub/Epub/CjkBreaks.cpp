#include "CjkBreaks.h"

#include <Utf8.h>

#include "TokenBoundary.h"

namespace {

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) {
    return true;
  }
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace CjkBreaks {

bool boundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return !isNoBreakBeforeCjkPunctuation(rightCp);
  }
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

// Korean separates words with spaces, so a boundary touching Hangul is not a gap-less break inside
// a line. hangulLineEndBreaks() still lets a Hangul word split there at a line end.
bool hasBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return boundaryAllowsBreak(leftCp, rightCp);
  }
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return boundaryAllowsBreak(leftCp, rightCp);
}

std::vector<size_t> characterBreakByteOffsets(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  uint32_t previous = utf8NextCodepoint(&ptr);
  if (previous == 0) return {};
  bool hasCjkBreakable = utf8IsCjkBreakable(previous);
  // Count first so words without accepted boundaries need no storage.
  size_t boundaryCount = 0;

  while (*ptr) {
    const uint32_t current = utf8NextCodepoint(&ptr);
    if (current == 0) break;
    if (utf8IsCjkBreakable(current)) {
      hasCjkBreakable = true;
    }
    if (hasBreakOpportunityBetween(previous, current)) {
      ++boundaryCount;
    }
    previous = current;
  }

  if (!hasCjkBreakable || boundaryCount == 0) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(boundaryCount);
  ptr = start;
  previous = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t current = utf8NextCodepoint(&ptr);
    if (current == 0) break;
    if (hasBreakOpportunityBetween(previous, current)) {
      allowedOffsets.push_back(offset);
    }
    previous = current;
  }
  return allowedOffsets;
}

}  // namespace CjkBreaks
