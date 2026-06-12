#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <string>

#include "lib/FsHelpers/FsHelpers.h"

namespace {

// Mirrors the on-device buffer: ScreenshotInfo::title is char[64] and the
// screenshot path component is sanitized into a char[64].
constexpr size_t kBuf = 64;

std::string sanitize(const std::string& input) {
  char out[kBuf];
  FsHelpers::sanitizePathComponentForFat32(input.c_str(), out, sizeof(out));
  return std::string(out);
}

// Reproduces getScreenshotInfo()'s `snprintf(info.title, 64, "%s", title)`,
// which truncates at a byte boundary and can split a multi-byte glyph before
// the string ever reaches the sanitizer.
std::string truncateLikeSnprintf(const std::string& title) {
  char buf[kBuf];
  std::snprintf(buf, sizeof(buf), "%s", title.c_str());
  return std::string(buf);
}

// Returns true if `s` is well-formed UTF-8. SdFat with USE_UTF8_LONG_NAMES
// rejects an incomplete sequence, which is what broke screenshot saving.
bool isValidUtf8(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) {
    int trailing;
    const unsigned char c = *p;
    if (c < 0x80) {
      trailing = 0;
    } else if ((c & 0xE0) == 0xC0) {
      trailing = 1;
    } else if ((c & 0xF0) == 0xE0) {
      trailing = 2;
    } else if ((c & 0xF8) == 0xF0) {
      trailing = 3;
    } else {
      return false;
    }
    for (int k = 0; k < trailing; k++) {
      if ((*++p & 0xC0) != 0x80) return false;
    }
    p++;
  }
  return true;
}

// Cyrillic titles from the bug reports (#2103, #2199). Both exceed the 64-byte
// buffer, so the on-device snprintf splits the final two-byte glyph.
const char kTitle2103[] = "Богиня глюкозы. Нормализуйте уровень сахара в крови, чтобы изменить свою жизнь";
const char kTitle2199[] = "Вглядываясь в солнце. Жизнь без страха смерти";
const char kTitle2199Works[] = "Эдем (полный перевод)";

}  // namespace

// The regression: even when the input was already truncated mid-character
// upstream (snprintf into char[64]), the sanitizer must still emit valid UTF-8.
// Before the fix the dangling lead byte survived and SdFat rejected the path.
TEST(SanitizeFat32, PreTruncatedTitleYieldsValidUtf8) {
  for (const char* title : {kTitle2103, kTitle2199}) {
    const std::string preTruncated = truncateLikeSnprintf(title);
    ASSERT_FALSE(isValidUtf8(preTruncated)) << "expected snprintf to split a glyph for: " << title;
    const std::string out = sanitize(preTruncated);
    EXPECT_TRUE(isValidUtf8(out)) << "sanitized output is invalid UTF-8 for: " << title;
    EXPECT_FALSE(out.empty());
  }
}

// The case the original fix already covered: the sanitizer itself truncates an
// over-long input at maxLen, splitting a glyph at the boundary.
TEST(SanitizeFat32, InFunctionTruncationYieldsValidUtf8) {
  for (const char* title : {kTitle2103, kTitle2199}) {
    EXPECT_TRUE(isValidUtf8(sanitize(title)));
  }
}

// Titles that fit must pass through untouched apart from character replacement.
TEST(SanitizeFat32, ShortTitleUnchanged) {
  EXPECT_EQ(sanitize(kTitle2199Works), "Эдем-(полный-перевод)");
  EXPECT_TRUE(isValidUtf8(sanitize(kTitle2199Works)));
}

// Complete multi-byte characters at the tail must be preserved, including a
// four-byte sequence — the trim must not eat a valid trailing glyph.
TEST(SanitizeFat32, CompleteTrailingCharactersPreserved) {
  EXPECT_EQ(sanitize("ab\xF0\x9F\x98\x80"), "ab\xF0\x9F\x98\x80");  // "ab😀"
  EXPECT_EQ(sanitize("Эдем"), "Эдем");
}

// Reserved FAT characters, spaces, and control codes become '-'.
TEST(SanitizeFat32, ReservedCharactersReplaced) { EXPECT_EQ(sanitize("a/b:c*d?e\"f<g>h|i j"), "a-b-c-d-e-f-g-h-i-j"); }

// Degenerate inputs must not read or write out of bounds.
TEST(SanitizeFat32, EmptyAndZeroLength) {
  char out[kBuf];
  FsHelpers::sanitizePathComponentForFat32("", out, sizeof(out));
  EXPECT_STREQ(out, "");

  // maxLen == 0 must be a no-op (no write).
  char guard[2] = {'X', 'X'};
  FsHelpers::sanitizePathComponentForFat32("abc", guard, 0);
  EXPECT_EQ(guard[0], 'X');
}
