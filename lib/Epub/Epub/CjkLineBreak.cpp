#include "CjkLineBreak.h"

#include <Utf8.h>

#include "TokenBoundary.h"

namespace CjkLineBreak {

bool isNoLineStart(const uint32_t cp) {
  // A dash or an ellipsis never starts a line after a CJK character (- 058A 2010 2012-2015 2043
  // 207B 208B 2212 2E17 2E3A 2E3B FE58 FE63 FF0D _ ...), so a run of them never splits either.
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
    // Ours, from crengine (UAX #14 CL, EX, NS and IN):
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF60:  // ｠
    case 0xFE50:  // ﹐
    case 0xFE52:  // ﹒
    case 0xFE56:  // ﹖
    case 0xFE57:  // ﹗
    case 0xFF61:  // ｡
    case 0xFF63:  // ｣
    case 0xFF64:  // ､
    case 0xFF65:  // ･
    case 0x3005:  // 々
    case 0x303B:  // 〻
    case 0x309D:  // ゝ
    case 0x309E:  // ゞ
    case 0x30FD:  // ヽ
    case 0x30FE:  // ヾ
    case 0x30FB:  // ・
    case 0x00B7:  // · (ours, not crengine's: transliterated names in Chinese, 列夫·托尔斯泰)
    case 0x301C:  // 〜
    case 0x30A0:  // ゠
    case 0x203C:  // ‼
    case 0x203D:  // ‽
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
    case 0x309B:  // ゛
    case 0x309C:  // ゜
    case 0xFF9E:  // ﾞ
    case 0xFF9F:  // ﾟ
    case 0x303C:  // 〼
    case 0x2025:  // ‥
    case 0x2024:  // ․
    // Ours: a straight quote may open or close a quotation, so it never breaks on either side
    // (UAX #14 LB19, class QU); it is in isNoLineEnd too.
    case '"':
    case '\'':
    case 0xFF02:  // ＂
    case 0xFF07:  // ＇
    // Ours: no break before the ideographic space (UAX #14 BA), so ！　 never leaves a line starting
    // with what reads as a paragraph indent.
    case 0x3000:  //
      return true;
    default:
      return false;
  }
}

bool isNoLineEnd(const uint32_t cp) {
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
    // Ours, from crengine (UAX #14 OP):
    case 0x301D:  // 〝
    case 0xFF5F:  // ｟
    case 0xFF62:  // ｢
    case 0x201E:  // „
    case 0x201A:  // ‚
    // Ours: a straight quote may open or close a quotation, so it never breaks on either side
    // (UAX #14 LB19, class QU); it is in isNoLineStart too.
    case '"':
    case '\'':
    case 0xFF02:  // ＂
    case 0xFF07:  // ＇
      return true;
    default:
      return false;
  }
}

bool boundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return !isNoLineStart(rightCp);
  }
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoLineEnd(leftCp) || isNoLineStart(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

bool isGapBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return boundaryAllowsBreak(leftCp, rightCp);
  }
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return boundaryAllowsBreak(leftCp, rightCp);
}

std::vector<size_t> breakByteOffsets(const std::string& text) {
  // First pass allocates nothing: most words are Latin and return here.
  bool hasCjk = false;
  size_t count = 0;
  {
    const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
    while (*ptr) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) break;
      if (utf8IsCjkBreakable(cp)) hasCjk = true;
      ++count;
    }
  }
  if (!hasCjk || count < 2) return {};

  std::vector<size_t> offsets;
  offsets.reserve(count - 1);
  const auto* const start = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t prevEnd = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if (cur == 0) break;
    if (isGapBetween(prev, cur)) offsets.push_back(prevEnd);
    prev = cur;
  }
  return offsets;
}

std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (word.empty()) return breaks;
  const auto* const start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if ((utf8IsHangul(prev) || utf8IsHangul(cur)) && boundaryAllowsBreak(prev, cur)) {
      breaks.push_back({offset, false});
    }
    prev = cur;
  }
  return breaks;
}

}  // namespace CjkLineBreak
