#include "VietnameseTelex.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "Utf8.h"

namespace VietnameseTelex {
namespace {
constexpr size_t kMaxTokenCodepoints = 16;
constexpr int kToneNone = 0;

enum class VowelGroup : uint8_t { A, AW, AA, E, EE, I, O, OO, OW, U, UW, Y, Count };

constexpr char32_t kLowerVowels[static_cast<size_t>(VowelGroup::Count)][6] = {
    {U'a', U'á', U'à', U'ả', U'ã', U'ạ'}, {U'ă', U'ắ', U'ằ', U'ẳ', U'ẵ', U'ặ'},
    {U'â', U'ấ', U'ầ', U'ẩ', U'ẫ', U'ậ'}, {U'e', U'é', U'è', U'ẻ', U'ẽ', U'ẹ'},
    {U'ê', U'ế', U'ề', U'ể', U'ễ', U'ệ'}, {U'i', U'í', U'ì', U'ỉ', U'ĩ', U'ị'},
    {U'o', U'ó', U'ò', U'ỏ', U'õ', U'ọ'}, {U'ô', U'ố', U'ồ', U'ổ', U'ỗ', U'ộ'},
    {U'ơ', U'ớ', U'ờ', U'ở', U'ỡ', U'ợ'}, {U'u', U'ú', U'ù', U'ủ', U'ũ', U'ụ'},
    {U'ư', U'ứ', U'ừ', U'ử', U'ữ', U'ự'}, {U'y', U'ý', U'ỳ', U'ỷ', U'ỹ', U'ỵ'},
};

constexpr char32_t kUpperVowels[static_cast<size_t>(VowelGroup::Count)][6] = {
    {U'A', U'Á', U'À', U'Ả', U'Ã', U'Ạ'}, {U'Ă', U'Ắ', U'Ằ', U'Ẳ', U'Ẵ', U'Ặ'},
    {U'Â', U'Ấ', U'Ầ', U'Ẩ', U'Ẫ', U'Ậ'}, {U'E', U'É', U'È', U'Ẻ', U'Ẽ', U'Ẹ'},
    {U'Ê', U'Ế', U'Ề', U'Ể', U'Ễ', U'Ệ'}, {U'I', U'Í', U'Ì', U'Ỉ', U'Ĩ', U'Ị'},
    {U'O', U'Ó', U'Ò', U'Ỏ', U'Õ', U'Ọ'}, {U'Ô', U'Ố', U'Ồ', U'Ổ', U'Ỗ', U'Ộ'},
    {U'Ơ', U'Ớ', U'Ờ', U'Ở', U'Ỡ', U'Ợ'}, {U'U', U'Ú', U'Ù', U'Ủ', U'Ũ', U'Ụ'},
    {U'Ư', U'Ứ', U'Ừ', U'Ử', U'Ữ', U'Ự'}, {U'Y', U'Ý', U'Ỳ', U'Ỷ', U'Ỹ', U'Ỵ'},
};

struct VowelInfo {
  VowelGroup group = VowelGroup::A;
  int tone = kToneNone;
  bool upper = false;
};

struct Token {
  size_t startByte = 0;
  size_t endByte = 0;
  size_t cursorIndex = 0;
  std::vector<char32_t> codepoints;
};

bool decodeVowel(const char32_t cp, VowelInfo& info) {
  for (size_t group = 0; group < static_cast<size_t>(VowelGroup::Count); ++group) {
    for (int tone = 0; tone < 6; ++tone) {
      if (kLowerVowels[group][tone] == cp) {
        info = {static_cast<VowelGroup>(group), tone, false};
        return true;
      }
      if (kUpperVowels[group][tone] == cp) {
        info = {static_cast<VowelGroup>(group), tone, true};
        return true;
      }
    }
  }
  return false;
}

char32_t encodeVowel(const VowelInfo& info) {
  const size_t group = static_cast<size_t>(info.group);
  return info.upper ? kUpperVowels[group][info.tone] : kLowerVowels[group][info.tone];
}

bool decodeCodepoint(const std::string& text, const size_t at, const size_t end, char32_t& out, size_t& next) {
  if (at >= end) return false;
  const auto lead = static_cast<unsigned char>(text[at]);
  size_t length = 0;
  char32_t value = 0;
  if (lead < 0x80U) {
    length = 1;
    value = lead;
  } else if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    value = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    value = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    value = lead & 0x07U;
  } else {
    return false;
  }
  if (at + length > end) return false;
  for (size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[at + index]);
    if ((byte & 0xC0U) != 0x80U) return false;
    value = (value << 6U) | (byte & 0x3FU);
  }
  const bool overlong = (length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) ||
                        (length == 4 && value < 0x10000U);
  if (overlong || (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) return false;
  out = value;
  next = at + length;
  return true;
}

size_t previousBoundary(const std::string& text, size_t at) {
  if (at == 0) return 0;
  --at;
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0U) == 0x80U) --at;
  return at;
}

bool isCursorBoundary(const std::string& text, const size_t at) {
  return at <= text.size() && (at == text.size() || (static_cast<unsigned char>(text[at]) & 0xC0U) != 0x80U);
}

bool isTokenLetter(const char32_t cp) {
  VowelInfo ignored;
  return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') || cp == U'đ' || cp == U'Đ' ||
         decodeVowel(cp, ignored);
}

bool readToken(const std::string& text, const size_t cursor, Token& token) {
  size_t start = cursor;
  size_t beforeCount = 0;
  while (start > 0) {
    const size_t previous = previousBoundary(text, start);
    char32_t cp = 0;
    size_t next = 0;
    if (!decodeCodepoint(text, previous, start, cp, next) || next != start || !isTokenLetter(cp)) break;
    if (++beforeCount > kMaxTokenCodepoints) return false;
    start = previous;
  }

  size_t end = cursor;
  size_t afterCount = 0;
  while (end < text.size()) {
    char32_t cp = 0;
    size_t next = 0;
    if (!decodeCodepoint(text, end, text.size(), cp, next) || !isTokenLetter(cp)) break;
    if (++afterCount + beforeCount > kMaxTokenCodepoints) return false;
    end = next;
  }

  token = {};
  token.startByte = start;
  token.endByte = end;
  size_t at = start;
  while (at < end) {
    char32_t cp = 0;
    size_t next = 0;
    if (!decodeCodepoint(text, at, end, cp, next)) return false;
    if (at < cursor) ++token.cursorIndex;
    token.codepoints.push_back(cp);
    at = next;
  }
  return true;
}

bool isAscii(const char32_t cp, const char lower) {
  return cp == static_cast<char32_t>(lower) || cp == static_cast<char32_t>(std::toupper(lower));
}

bool isShaped(const VowelGroup group) {
  return group == VowelGroup::AW || group == VowelGroup::AA || group == VowelGroup::EE ||
         group == VowelGroup::OO || group == VowelGroup::OW || group == VowelGroup::UW;
}

VowelGroup unshapedGroup(const VowelGroup group) {
  switch (group) {
    case VowelGroup::AW:
    case VowelGroup::AA:
      return VowelGroup::A;
    case VowelGroup::EE:
      return VowelGroup::E;
    case VowelGroup::OO:
    case VowelGroup::OW:
      return VowelGroup::O;
    case VowelGroup::UW:
      return VowelGroup::U;
    default:
      return group;
  }
}

std::vector<size_t> toneVowels(const std::vector<char32_t>& cps) {
  std::vector<size_t> vowels;
  for (size_t index = 0; index < cps.size(); ++index) {
    VowelInfo info;
    if (decodeVowel(cps[index], info)) vowels.push_back(index);
  }
  if (vowels.size() > 1) {
    if (vowels.front() > 0 && isAscii(cps[vowels.front() - 1], 'q')) {
      VowelInfo info;
      decodeVowel(cps[vowels.front()], info);
      if (unshapedGroup(info.group) == VowelGroup::U) vowels.erase(vowels.begin());
    } else if (vowels.front() > 0 && isAscii(cps[vowels.front() - 1], 'g')) {
      VowelInfo info;
      decodeVowel(cps[vowels.front()], info);
      if (info.group == VowelGroup::I) vowels.erase(vowels.begin());
    }
  }
  return vowels;
}

size_t toneTarget(const std::vector<char32_t>& cps, const std::vector<size_t>& vowels) {
  for (auto iterator = vowels.rbegin(); iterator != vowels.rend(); ++iterator) {
    VowelInfo info;
    decodeVowel(cps[*iterator], info);
    if (isShaped(info.group)) return *iterator;
  }
  if (vowels.size() == 1) return vowels.front();
  if (vowels.size() >= 3) return vowels[vowels.size() / 2];
  const bool hasFollowingConsonant = vowels.back() + 1 < cps.size();
  return hasFollowingConsonant ? vowels.back() : vowels.front();
}

int currentTone(const std::vector<char32_t>& cps) {
  for (const char32_t cp : cps) {
    VowelInfo info;
    if (decodeVowel(cp, info) && info.tone != kToneNone) return info.tone;
  }
  return kToneNone;
}

bool hasContiguousVowelNucleus(const std::vector<char32_t>& cps) {
  bool sawVowel = false;
  bool nucleusEnded = false;
  for (const char32_t cp : cps) {
    VowelInfo info;
    if (decodeVowel(cp, info)) {
      if (nucleusEnded) return false;
      sawVowel = true;
    } else if (sawVowel && isTokenLetter(cp)) {
      nucleusEnded = true;
    }
  }
  return sawVowel;
}

bool clearDiacritics(std::vector<char32_t>& cps) {
  bool removed = false;
  for (char32_t& cp : cps) {
    VowelInfo info;
    if (decodeVowel(cp, info)) {
      const VowelGroup plain = unshapedGroup(info.group);
      if (info.tone != kToneNone || plain != info.group) {
        info.tone = kToneNone;
        info.group = plain;
        cp = encodeVowel(info);
        removed = true;
      }
    } else if (cp == U'đ' || cp == U'Đ') {
      cp = cp == U'Đ' ? U'D' : U'd';
      removed = true;
    }
  }
  return removed;
}

bool normalizeTone(std::vector<char32_t>& cps) {
  const int tone = currentTone(cps);
  if (tone == kToneNone) return false;
  const auto vowels = toneVowels(cps);
  if (vowels.empty()) return false;
  const size_t target = toneTarget(cps, vowels);
  bool changed = false;
  for (const size_t index : vowels) {
    VowelInfo info;
    decodeVowel(cps[index], info);
    const int wanted = index == target ? tone : kToneNone;
    if (info.tone != wanted) {
      info.tone = wanted;
      cps[index] = encodeVowel(info);
      changed = true;
    }
  }
  return changed;
}

void clearTone(std::vector<char32_t>& cps) {
  for (char32_t& cp : cps) {
    VowelInfo info;
    if (decodeVowel(cp, info) && info.tone != kToneNone) {
      info.tone = kToneNone;
      cp = encodeVowel(info);
    }
  }
}

bool applySimpleShape(std::vector<char32_t>& cps, const size_t cursorIndex, const VowelGroup plain,
                      const VowelGroup shaped, bool& escaped) {
  if (cursorIndex == 0) return false;
  VowelInfo info;
  if (!decodeVowel(cps[cursorIndex - 1], info)) return false;
  if (info.group == plain) {
    info.group = shaped;
    cps[cursorIndex - 1] = encodeVowel(info);
    return true;
  }
  if (info.group == shaped) {
    info.group = plain;
    cps[cursorIndex - 1] = encodeVowel(info);
    escaped = true;
    return true;
  }
  return false;
}

bool applyWShape(std::vector<char32_t>& cps, const size_t cursorIndex, bool& escaped) {
  std::vector<size_t> vowels;
  for (size_t index = 0; index < cursorIndex; ++index) {
    VowelInfo info;
    if (decodeVowel(cps[index], info)) vowels.push_back(index);
  }
  if (vowels.size() >= 2) {
    const size_t firstIndex = vowels[vowels.size() - 2];
    const size_t secondIndex = vowels.back();
    VowelInfo first;
    VowelInfo second;
    decodeVowel(cps[firstIndex], first);
    decodeVowel(cps[secondIndex], second);
    if (first.group == VowelGroup::U && second.group == VowelGroup::O) {
      first.group = VowelGroup::UW;
      second.group = VowelGroup::OW;
      cps[firstIndex] = encodeVowel(first);
      cps[secondIndex] = encodeVowel(second);
      return true;
    }
    if (first.group == VowelGroup::UW && second.group == VowelGroup::OW) {
      first.group = VowelGroup::U;
      second.group = VowelGroup::O;
      cps[firstIndex] = encodeVowel(first);
      cps[secondIndex] = encodeVowel(second);
      escaped = true;
      return true;
    }
  }
  for (size_t index = cursorIndex; index > 0; --index) {
    VowelInfo info;
    if (!decodeVowel(cps[index - 1], info)) continue;
    VowelGroup target;
    if (info.group == VowelGroup::A) {
      target = VowelGroup::AW;
    } else if (info.group == VowelGroup::O) {
      target = VowelGroup::OW;
    } else if (info.group == VowelGroup::U) {
      target = VowelGroup::UW;
    } else if (info.group == VowelGroup::AW || info.group == VowelGroup::OW || info.group == VowelGroup::UW) {
      target = unshapedGroup(info.group);
      escaped = true;
    } else {
      continue;
    }
    info.group = target;
    cps[index - 1] = encodeVowel(info);
    return true;
  }
  return false;
}

std::string encodeToken(const std::vector<char32_t>& cps, const size_t count) {
  std::string result;
  for (size_t index = 0; index < std::min(count, cps.size()); ++index) {
    utf8AppendCodepoint(cps[index], result);
  }
  return result;
}

bool publishToken(std::string& text, size_t& cursor, const Token& original, const std::vector<char32_t>& cps,
                  const size_t cursorIndex, const size_t maxBytes) {
  const std::string replacement = encodeToken(cps, cps.size());
  std::string candidate = text;
  candidate.replace(original.startByte, original.endByte - original.startByte, replacement);
  if (maxBytes != 0 && candidate.size() > maxBytes) return false;
  cursor = original.startByte + encodeToken(cps, cursorIndex).size();
  text = std::move(candidate);
  return true;
}

bool insertLiteral(std::string& text, size_t& cursor, const char key, const size_t maxBytes) {
  if (maxBytes != 0 && text.size() + 1 > maxBytes) return false;
  text.insert(cursor, 1, key);
  ++cursor;
  return true;
}

int toneForKey(const char lower) {
  switch (lower) {
    case 's':
      return 1;
    case 'f':
      return 2;
    case 'r':
      return 3;
    case 'x':
      return 4;
    case 'j':
      return 5;
    default:
      return kToneNone;
  }
}
}  // namespace

bool applyKey(std::string& text, size_t& cursorByte, const char asciiKey, const size_t maxBytes) {
  if (!isCursorBoundary(text, cursorByte) || static_cast<unsigned char>(asciiKey) >= 0x80U || asciiKey == '\0') {
    return false;
  }
  const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(asciiKey)));
  if (!std::isalpha(static_cast<unsigned char>(asciiKey))) {
    return insertLiteral(text, cursorByte, asciiKey, maxBytes);
  }

  Token token;
  if (!readToken(text, cursorByte, token)) return insertLiteral(text, cursorByte, asciiKey, maxBytes);
  auto cps = token.codepoints;
  size_t newCursorIndex = token.cursorIndex;
  bool handled = false;
  bool escaped = false;
  const bool validNucleus = hasContiguousVowelNucleus(cps);

  if (lower == 'a' && validNucleus) {
    handled = applySimpleShape(cps, token.cursorIndex, VowelGroup::A, VowelGroup::AA, escaped);
  } else if (lower == 'e' && validNucleus) {
    handled = applySimpleShape(cps, token.cursorIndex, VowelGroup::E, VowelGroup::EE, escaped);
  } else if (lower == 'o' && validNucleus) {
    handled = applySimpleShape(cps, token.cursorIndex, VowelGroup::O, VowelGroup::OO, escaped);
  } else if (lower == 'd' && token.cursorIndex > 0) {
    char32_t& previous = cps[token.cursorIndex - 1];
    if (previous == U'd' || previous == U'D') {
      previous = previous == U'D' ? U'Đ' : U'đ';
      handled = true;
    } else if (previous == U'đ' || previous == U'Đ') {
      previous = previous == U'Đ' ? U'D' : U'd';
      handled = true;
      escaped = true;
    }
  } else if (lower == 'w' && (validNucleus || token.codepoints.empty())) {
    handled = applyWShape(cps, token.cursorIndex, escaped);
    if (!handled && token.codepoints.empty()) {
      cps.insert(cps.begin(), std::isupper(static_cast<unsigned char>(asciiKey)) ? U'Ư' : U'ư');
      newCursorIndex = 1;
      handled = true;
    }
  }

  const int requestedTone = toneForKey(lower);
  if (!handled && requestedTone != kToneNone && validNucleus) {
    const auto vowels = toneVowels(cps);
    if (!vowels.empty()) {
      const int existingTone = currentTone(cps);
      clearTone(cps);
      if (existingTone == requestedTone) {
        escaped = true;
      } else {
        VowelInfo target;
        const size_t targetIndex = toneTarget(cps, vowels);
        decodeVowel(cps[targetIndex], target);
        target.tone = requestedTone;
        cps[targetIndex] = encodeVowel(target);
      }
      handled = true;
    }
  }

  if (!handled && lower == 'z') {
    handled = clearDiacritics(cps);
  }

  if (!handled || escaped) {
    cps.insert(cps.begin() + static_cast<std::ptrdiff_t>(token.cursorIndex), static_cast<unsigned char>(asciiKey));
    newCursorIndex = token.cursorIndex + 1;
  }
  normalizeTone(cps);
  return publishToken(text, cursorByte, token, cps, newCursorIndex, maxBytes);
}

bool normalizeAtCursor(std::string& text, size_t& cursorByte, const size_t maxBytes) {
  if (!isCursorBoundary(text, cursorByte)) return false;
  Token token;
  if (!readToken(text, cursorByte, token)) return false;
  auto cps = token.codepoints;
  if (!normalizeTone(cps)) return true;
  return publishToken(text, cursorByte, token, cps, token.cursorIndex, maxBytes);
}

}  // namespace VietnameseTelex
