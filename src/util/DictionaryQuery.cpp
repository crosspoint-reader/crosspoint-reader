#include "DictionaryQuery.h"

#include <Utf8.h>

#include <cctype>

namespace DictionaryQuery {
namespace {

bool isWordCodepoint(const uint32_t cp) {
  if (cp < 0x80) return std::isalnum(static_cast<unsigned char>(cp)) != 0;
  if (utf8IsCombiningMark(cp)) return false;
  return cp < 0x2000 || cp > 0x206F;
}

uint32_t lowerVietnamese(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
  if (cp >= 0x1EA0 && cp <= 0x1EF8 && (cp & 1u) == 0) return cp + 1;
  switch (cp) {
    case 0x00C0: return 0x00E0;
    case 0x00C1: return 0x00E1;
    case 0x00C2: return 0x00E2;
    case 0x00C3: return 0x00E3;
    case 0x00C8: return 0x00E8;
    case 0x00C9: return 0x00E9;
    case 0x00CA: return 0x00EA;
    case 0x00CC: return 0x00EC;
    case 0x00CD: return 0x00ED;
    case 0x00D2: return 0x00F2;
    case 0x00D3: return 0x00F3;
    case 0x00D4: return 0x00F4;
    case 0x00D5: return 0x00F5;
    case 0x00D9: return 0x00F9;
    case 0x00DA: return 0x00FA;
    case 0x00DD: return 0x00FD;
    case 0x0102: return 0x0103;
    case 0x0110: return 0x0111;
    case 0x0128: return 0x0129;
    case 0x0168: return 0x0169;
    case 0x01A0: return 0x01A1;
    case 0x01AF: return 0x01B0;
    default: return cp;
  }
}

}  // namespace

std::string clean(const std::string_view text) {
  const std::string normalized = utf8ComposeNfc(std::string(text));
  size_t start = std::string::npos;
  size_t end = 0;
  const unsigned char* base = reinterpret_cast<const unsigned char*>(normalized.c_str());
  const unsigned char* scan = base;
  while (*scan) {
    const size_t codepointStart = static_cast<size_t>(scan - base);
    const uint32_t cp = utf8NextCodepoint(&scan);
    if (!isWordCodepoint(cp)) continue;
    if (start == std::string::npos) start = codepointStart;
    end = static_cast<size_t>(scan - base);
  }
  if (start == std::string::npos || start >= end) return {};

  std::string result;
  result.reserve(end - start);
  const unsigned char* cursor = base + start;
  const unsigned char* finish = base + end;
  while (cursor < finish) {
    utf8AppendCodepoint(lowerVietnamese(utf8NextCodepoint(&cursor)), result);
  }
  return result;
}

bool buildPhrase(const char* const* words, const size_t count, std::string& out) {
  out.clear();
  if (!words || count == 0 || count > MAX_PHRASE_TOKENS) return false;

  for (size_t i = 0; i < count; i++) {
    const std::string token = clean(words[i] ? words[i] : "");
    if (token.empty()) return false;
    const size_t separator = out.empty() ? 0 : 1;
    if (out.size() + separator + token.size() > MAX_QUERY_BYTES) {
      out.clear();
      return false;
    }
    if (separator) out.push_back(' ');
    out += token;
  }
  return true;
}

}  // namespace DictionaryQuery
