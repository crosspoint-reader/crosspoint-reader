#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/ThaiDict/ThaiDict.h"

namespace {

// Decode a UTF-8 literal into codepoints (tests only — small and simple).
std::vector<uint32_t> cps(const char* utf8) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p >> 5) == 0x6) {
      cp = (*p++ & 0x1F) << 6;
      cp |= (*p++ & 0x3F);
    } else if ((*p >> 4) == 0xE) {
      cp = (*p++ & 0x0F) << 12;
      cp |= (*p++ & 0x3F) << 6;
      cp |= (*p++ & 0x3F);
    } else {
      p++;
    }
    out.push_back(cp);
  }
  return out;
}

size_t match(const char* utf8) {
  const auto v = cps(utf8);
  return ThaiDict::matchLongest(v.data(), v.size());
}

}  // namespace

TEST(ThaiDictMatchLongest, FindsExactWords) {
  EXPECT_EQ(match("กก"), 2u);     // first word in the list
  EXPECT_EQ(match("โลก"), 3u);    // world
  EXPECT_EQ(match("มนุษย์"), 6u);   // human (ends in thanthakhat)
  EXPECT_EQ(match("น้ำตา"), 5u);   // tears (sara am inside)
  EXPECT_EQ(match("หนังสือ"), 7u);  // book
}

TEST(ThaiDictMatchLongest, PicksLongestPrefix) {
  // ตา, ตาก, ตากลม are all words: the longest must win.
  EXPECT_EQ(match("ตากลม"), 5u);
  // โลก followed by more text still matches just the word.
  EXPECT_EQ(match("โลกใบนี้"), 3u);
  // น้ำ vs น้ำตา: longest wins when the continuation completes a word.
  EXPECT_EQ(match("น้ำตาไหล"), 5u);
}

TEST(ThaiDictMatchLongest, RejectsNonWords) {
  EXPECT_EQ(match("ฮุ่ยซัว"), 0u);   // transliterated name, not in the wordlist
  EXPECT_EQ(match("hello"), 0u);  // Latin never matches
  EXPECT_EQ(match(""), 0u);
}

TEST(ThaiDictMatchLongest, StopsAtNonThai) {
  // Thai word followed by Latin: the match window ends at the script switch.
  EXPECT_EQ(match("โลกabc"), 3u);
}
