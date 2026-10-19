#include "CjkPunctuation.h"

namespace CjkPunctuation {

using Table = TypographyProfile::Punctuation;

Class classOf(const uint32_t cp) {
  switch (cp) {
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0xFF08:  // （
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x3018:  // 〘
    case 0x3016:  // 〖
    case 0xFF5F:  // ｟
    case 0x301D:  // 〝
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x201C:  // “
    case 0x2018:  // ‘
      return Class::Opening;
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0xFF09:  // ）
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x3019:  // 〙
    case 0x3017:  // 〗
    case 0xFF60:  // ｠
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x201D:  // ”
    case 0x2019:  // ’
      return Class::Closing;
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0x203C:  // ‼
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
      return Class::Dividing;
    case 0x30FB:  // ・
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
      return Class::MiddleDot;
    case 0x3002:  // 。
    case 0xFF0E:  // ．
      return Class::FullStop;
    case 0x3001:  // 、
    case 0xFF0C:  // ，
      return Class::Comma;
    case 0x3000:
      return Class::IdeographicSpace;
    case 0xFF02:  // ＂
    case 0xFF07:  // ＇
      return Class::AmbiguousQuote;
    default:
      return Class::None;
  }
}

bool isCurlyQuote(const uint32_t cp) { return cp == 0x201C || cp == 0x201D || cp == 0x2018 || cp == 0x2019; }

namespace {

bool closesOrSeparates(const Class c) {
  return c == Class::Closing || c == Class::Dividing || c == Class::MiddleDot || c == Class::FullStop ||
         c == Class::Comma;
}

// CLREQ, mainland books: punctuation is half-width at a line's start (opening) or end (the rest), and in a run of
// marks only one keeps its blank -- the first of a run of opening marks, the last of the rest. Between a closing
// and an opening mark half an em of blank stays, a quarter on each side. Elsewhere a mark may halve on demand.
// Also the table for the CJK punctuation of a book in any other language.
Width simplifiedChinese(const Class mark, const Class neighbour, const bool atLineEdge) {
  if (mark == Class::Opening) {
    if (atLineEdge) return {4, 4};
    if (neighbour == Class::Opening || neighbour == Class::AmbiguousQuote) return {4, 4};
    if (closesOrSeparates(neighbour)) return {6, 6};
    return {8, 4};
  }
  if (closesOrSeparates(mark)) {
    if (atLineEdge) return {4, 4};
    if (neighbour == Class::Opening) return {6, 6};
    if (closesOrSeparates(neighbour) || neighbour == Class::AmbiguousQuote) return {4, 4};
    return {8, 4};
  }
  return {8, 8};
}

// CLREQ, Taiwan and Hong Kong: the marks are centred in their em, so they keep their full width, at a line's edges
// too. They give way only on demand: a bracket (ink on one side) to half next to punctuation or a line edge,
// everything else to three quarters.
Width traditionalChinese(const Class mark, const Class neighbour, const bool atLineEdge) {
  if (mark == Class::Opening) {
    if (atLineEdge) return {8, 4};
    return neighbour == Class::None || neighbour == Class::Closing ? Width{8, 6} : Width{8, 4};
  }
  if (mark == Class::Closing) {
    if (atLineEdge) return {8, 4};
    return neighbour == Class::None || neighbour == Class::Opening ? Width{8, 6} : Width{8, 4};
  }
  if (closesOrSeparates(mark) || mark == Class::AmbiguousQuote) return {8, 6};
  return {8, 8};
}

// JLREQ. An opening bracket at a line start, and a mark after 。、 or another opening bracket, sets solid (half).
// Between a closing and an opening bracket half an em of blank stays; elsewhere a quarter em of a bracket's blank
// may go on demand. 。 keeps its blank mid-line; 、 may halve. At a line end 。、 and a closing bracket are half or
// full (push-in decides, PR A3b). ！？ keep their full width but for a quarter on demand; ・：； may halve.
// Before a closing bracket or another of 。、・：；, a mark sets solid.
Width japanese(const Class mark, const Class neighbour, const bool atLineEdge) {
  switch (mark) {
    case Class::Opening:
      if (atLineEdge) return {4, 4};
      switch (neighbour) {
        case Class::Opening:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        case Class::Closing:
          return {6, 6};
        default:
          return {8, 6};
      }
    case Class::Closing:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Opening:
          return {6, 6};
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        default:
          return {8, 6};
      }
    case Class::Dividing:
      return {8, 6};
    case Class::MiddleDot:
      return {8, 4};
    case Class::FullStop:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
          return {4, 4};
        case Class::IdeographicSpace:
          return {8, 4};
        default:
          return {8, 6};
      }
    case Class::Comma:
      if (atLineEdge) return {8, 4};
      switch (neighbour) {
        case Class::Closing:
        case Class::MiddleDot:
        case Class::FullStop:
        case Class::Comma:
        case Class::AmbiguousQuote:
          return {4, 4};
        case Class::Opening:
          return {8, 6};
        default:
          return {8, 4};
      }
    default:
      return {8, 8};
  }
}

}  // namespace

Width widthOf(const Table table, const Class mark, const Class neighbour, const bool atLineEdge) {
  switch (table) {
    case Table::TraditionalChinese:
      return traditionalChinese(mark, neighbour, atLineEdge);
    case Table::Japanese:
      return japanese(mark, neighbour, atLineEdge);
    case Table::SimplifiedChinese:
    default:
      return simplifiedChinese(mark, neighbour, atLineEdge);
  }
}

}  // namespace CjkPunctuation
