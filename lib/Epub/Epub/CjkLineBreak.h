#pragma once

#include <Utf8.h>

#include <cstdint>

// Line-break prohibitions between adjacent CJK characters. ParsedText tokenizes CJK text one
// character at a time and consults hasCjkBreakOpportunityBetween() for every adjacent pair;
// a pair with no opportunity stays in one token, which is how a prohibited character is kept
// off the head or foot of a line. The classes follow JLREQ (W3C, Requirements for Japanese Text
// Layout): 3.1.7 lists what cannot start a line (cl-02 to cl-11, cl-13), 3.1.8 what cannot end
// one (cl-01, and cl-12 by the same reasoning), and cl-08 runs are not split. Members are taken
// from Appendix A. https://www.w3.org/TR/jlreq/
namespace CjkLineBreak {

inline bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
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
    // JLREQ 3.1.7 (Characters Not Starting a Line) lists these classes in the rule itself; the
    // original set carried only brackets and sentence punctuation. Members follow Appendix A.
    case 0x30FB:  // ・ (cl-05 middle dots)
    case 0xFF65:  // ･
    case 0x2010:  // ‐ (cl-03 hyphens)
    case 0x2013:  // –
    case 0x301C:  // 〜
    case 0x30A0:  // ゠
    case 0xFF5E:  // ～
    case 0x2014:  // — (cl-08 inseparable characters)
    case 0x2015:  // ―
    case 0x2025:  // ‥
    case 0x2026:  // …
    case 0x3005:  // 々 (cl-09 iteration marks)
    case 0x303B:  // 〻
    case 0x309D:  // ゝ
    case 0x309E:  // ゞ
    case 0x30FD:  // ヽ
    case 0x30FE:  // ヾ
    case 0x30FC:  // ー (cl-10 prolonged sound mark)
    case 0xFF70:  // ｰ
    case 0xFF9E:  // ﾞ (halfwidth voiced marks attach to the preceding kana)
    case 0xFF9F:  // ﾟ
    case 0x00B0:  // ° (cl-13 postfixed abbreviations)
    case 0x2030:  // ‰
    case 0x2032:  // ′
    case 0x2033:  // ″
    case 0x2103:  // ℃
    case 0xFF05:  // ％
    case 0x301F:  // 〟 (cl-02 closing quotation marks missing above)
    case 0xFF60:  // ｠
    case 0xFF61:  // ｡ (halfwidth full stop and comma)
    case 0xFF64:  // ､
    // cl-11 small kana: JLREQ 3.1.7 lists them in the rule itself; the Note records that some
    // books relax this together with 々 and ー, but the principle keeps them off the line head.
    case 0x3041:  // ぁ
    case 0x3043:  // ぃ
    case 0x3045:  // ぅ
    case 0x3047:  // ぇ
    case 0x3049:  // ぉ
    case 0x3063:  // っ
    case 0x3083:  // ゃ
    case 0x3085:  // ゅ
    case 0x3087:  // ょ
    case 0x308E:  // ゎ
    case 0x3095:  // ゕ
    case 0x3096:  // ゖ
    case 0x30A1:  // ァ
    case 0x30A3:  // ィ
    case 0x30A5:  // ゥ
    case 0x30A7:  // ェ
    case 0x30A9:  // ォ
    case 0x30C3:  // ッ
    case 0x30E3:  // ャ
    case 0x30E5:  // ュ
    case 0x30E7:  // ョ
    case 0x30EE:  // ヮ
    case 0x30F5:  // ヵ
    case 0x30F6:  // ヶ
    case 0x31F0:  // ㇰ .. ㇿ (Ainu small katakana)
    case 0x31F1:
    case 0x31F2:
    case 0x31F3:
    case 0x31F4:
    case 0x31F5:
    case 0x31F6:
    case 0x31F7:
    case 0x31F8:
    case 0x31F9:
    case 0x31FA:
    case 0x31FB:
    case 0x31FC:
    case 0x31FD:
    case 0x31FE:
    case 0x31FF:
    // cl-04 dividing punctuation beyond ! and ?
    case 0x203C:  // ‼
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
    // cl-13 postfixed abbreviations, the rest of Appendix A.13
    case 0x0025:  // %
    case 0x00A2:  // ¢
    case 0xFFE0:  // ￠
    case 0x2113:  // ℓ
    case 0x33CB:  // ㏋
    case 0x3303:  // ㌃ .. ㍄ squared unit symbols
    case 0x330D:
    case 0x3314:
    case 0x3318:
    case 0x3322:
    case 0x3323:
    case 0x3326:
    case 0x3327:
    case 0x332B:
    case 0x3336:
    case 0x333B:
    case 0x3349:
    case 0x334A:
    case 0x334D:
    case 0x3351:
    case 0x3357:
    case 0x338E:
    case 0x338F:
    case 0x339C:
    case 0x339D:
    case 0x339E:
    case 0x33A1:
    case 0x33C4:
      return true;
    default:
      return false;
  }
}

inline bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
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
    case 0x301D:  // 〝 (cl-01 opening quotation marks missing above)
    case 0xFF5F:  // ｟
    case 0x0024:  // $ (cl-12 prefixed abbreviations, JLREQ 3.1.8 / Appendix A.12: they belong to the number that
                  // follows)
    case 0x00A3:  // £
    case 0x00A5:  // ¥
    case 0x20AC:  // €
    case 0x2116:  // №
    case 0x0023:  // #
    case 0xFF03:  // ＃
    case 0xFF04:  // ＄
    case 0xFFE1:  // ￡
    case 0xFFE5:  // ￥
      return true;
    default:
      return false;
  }
}

// cl-08 inseparable characters: a run of the same leader or dash (……, ――) is one mark
// and must not be split. Only same-character pairs qualify; a leader followed by text
// may break after it.
inline bool isCjkInseparablePair(const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp != rightCp) return false;
  switch (leftCp) {
    case 0x2014:  // —
    case 0x2015:  // ―
    case 0x2025:  // ‥
    case 0x2026:  // …
    case 0x3033:  // 〳
    case 0x3034:  // 〴
    case 0x3035:  // 〵
      return true;
    default:
      return false;
  }
}

inline bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (isCjkInseparablePair(leftCp, rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

}  // namespace CjkLineBreak
