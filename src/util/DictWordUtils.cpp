#include "DictWordUtils.h"

#include <cctype>
#include <cstring>

namespace DictWordUtils {

namespace {

// ASCII alphanumerics plus any UTF-8 lead/continuation byte, so accented
// words keep their edges.
bool isWordByte(const unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }

// Lead bytes of a General Punctuation codepoint (U+2000-U+207F: E2 80 xx / E2 81 xx).
bool isGeneralPunctuation(const unsigned char* p) { return p[0] == 0xE2 && (p[1] == 0x80 || p[1] == 0x81); }

}  // namespace

std::string trimWordEdges(const char* word) {
  if (!word) return "";
  const auto* b = reinterpret_cast<const unsigned char*>(word);
  size_t start = 0;
  size_t end = strlen(word);
  while (start < end) {
    if (!isWordByte(b[start])) {
      start++;
    } else if (end - start >= 3 && isGeneralPunctuation(b + start)) {
      start += 3;
    } else {
      break;
    }
  }
  while (end > start) {
    if (!isWordByte(b[end - 1])) {
      end--;
    } else if (end - start >= 3 && isGeneralPunctuation(b + end - 3)) {
      end -= 3;
    } else {
      break;
    }
  }
  return start < end ? std::string(word + start, end - start) : std::string();
}

int headwordRank(const char* headword, const char* textWord) {
  if (strcmp(headword, textWord) == 0) return 0;
  return std::isupper(static_cast<unsigned char>(headword[0])) ? 2 : 1;
}

}  // namespace DictWordUtils
