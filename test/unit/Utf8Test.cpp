#include <Utf8.h>

#include "test/test_harness.h"

// --- utf8NextCodepoint ---

void testAscii() {
  const unsigned char data[] = "A";
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0x41u);
  ASSERT_EQ(static_cast<size_t>(p - data), 1u);
}

void testTwoByte() {
  // U+00E9 (é) = C3 A9
  const unsigned char data[] = {0xC3, 0xA9, 0x00};
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0x00E9u);
  ASSERT_EQ(static_cast<size_t>(p - data), 2u);
}

void testThreeByte() {
  // U+4E16 (世) = E4 B8 96
  const unsigned char data[] = {0xE4, 0xB8, 0x96, 0x00};
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0x4E16u);
  ASSERT_EQ(static_cast<size_t>(p - data), 3u);
}

void testFourByte() {
  // U+1F600 (😀) = F0 9F 98 80
  const unsigned char data[] = {0xF0, 0x9F, 0x98, 0x80, 0x00};
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0x1F600u);
  ASSERT_EQ(static_cast<size_t>(p - data), 4u);
}

void testNullTerminator() {
  const unsigned char data[] = {0x00};
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0u);
}

void testMultipleCodepoints() {
  // "Aé" = 41 C3 A9
  const unsigned char data[] = {0x41, 0xC3, 0xA9, 0x00};
  const unsigned char* p = data;
  ASSERT_EQ(utf8NextCodepoint(&p), 0x41u);
  ASSERT_EQ(utf8NextCodepoint(&p), 0xE9u);
  ASSERT_EQ(utf8NextCodepoint(&p), 0u);
}

// --- utf8RemoveLastChar ---

void testRemoveLastAscii() {
  std::string s = "abc";
  size_t newSize = utf8RemoveLastChar(s);
  ASSERT_EQ(newSize, 2u);
  ASSERT_STREQ(s, "ab");
}

void testRemoveLastMultibyte() {
  // "aé" = 61 C3 A9
  std::string s = "a\xC3\xA9";
  size_t newSize = utf8RemoveLastChar(s);
  ASSERT_EQ(newSize, 1u);
  ASSERT_STREQ(s, "a");
}

void testRemoveLastEmpty() {
  std::string s;
  size_t newSize = utf8RemoveLastChar(s);
  ASSERT_EQ(newSize, 0u);
  ASSERT_TRUE(s.empty());
}

void testRemoveLastSingleChar() {
  std::string s = "X";
  utf8RemoveLastChar(s);
  ASSERT_TRUE(s.empty());
}

// --- utf8TruncateChars ---

void testTruncateZero() {
  std::string s = "hello";
  utf8TruncateChars(s, 0);
  ASSERT_STREQ(s, "hello");
}

void testTruncateOne() {
  std::string s = "hello";
  utf8TruncateChars(s, 1);
  ASSERT_STREQ(s, "hell");
}

void testTruncateAll() {
  std::string s = "hi";
  utf8TruncateChars(s, 5);  // more than string length
  ASSERT_TRUE(s.empty());
}

void testTruncateMultibyte() {
  // "aéb" — remove 2 chars from end → "a"
  std::string s =
      "a\xC3\xA9"
      "b";
  utf8TruncateChars(s, 2);
  ASSERT_STREQ(s, "a");
}

int main() {
  std::cout << "Utf8Test\n";
  RUN_TEST(testAscii);
  RUN_TEST(testTwoByte);
  RUN_TEST(testThreeByte);
  RUN_TEST(testFourByte);
  RUN_TEST(testNullTerminator);
  RUN_TEST(testMultipleCodepoints);
  RUN_TEST(testRemoveLastAscii);
  RUN_TEST(testRemoveLastMultibyte);
  RUN_TEST(testRemoveLastEmpty);
  RUN_TEST(testRemoveLastSingleChar);
  RUN_TEST(testTruncateZero);
  RUN_TEST(testTruncateOne);
  RUN_TEST(testTruncateAll);
  RUN_TEST(testTruncateMultibyte);
  TEST_SUMMARY();
}
