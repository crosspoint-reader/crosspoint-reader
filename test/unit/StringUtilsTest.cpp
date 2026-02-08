#include "src/util/StringUtils.h"
#include "test/test_harness.h"

// --- sanitizeFilename ---

void testSanitizeNormal() { ASSERT_STREQ(StringUtils::sanitizeFilename("my_book"), "my_book"); }

void testSanitizeInvalidChars() {
  ASSERT_STREQ(StringUtils::sanitizeFilename("file/name:bad"), "file_name_bad");
  ASSERT_STREQ(StringUtils::sanitizeFilename("a*b?c\"d<e>f|g"), "a_b_c_d_e_f_g");
}

void testSanitizeTrimSpacesDots() { ASSERT_STREQ(StringUtils::sanitizeFilename("  ..hello..  "), "hello"); }

void testSanitizeAllInvalid() { ASSERT_STREQ(StringUtils::sanitizeFilename("..."), "book"); }

void testSanitizeMaxLength() {
  std::string longName(200, 'a');
  std::string result = StringUtils::sanitizeFilename(longName, 50);
  ASSERT_EQ(result.length(), 50u);
}

void testSanitizeEmpty() { ASSERT_STREQ(StringUtils::sanitizeFilename(""), "book"); }

void testSanitizeNonPrintable() {
  std::string s;
  s += '\x01';
  s += '\x02';
  s += '\x1F';
  ASSERT_STREQ(StringUtils::sanitizeFilename(s), "book");
}

// --- checkFileExtension (std::string) ---

void testExtensionEpub() {
  ASSERT_TRUE(StringUtils::checkFileExtension(std::string("book.epub"), ".epub"));
  ASSERT_TRUE(StringUtils::checkFileExtension(std::string("book.EPUB"), ".epub"));
  ASSERT_TRUE(StringUtils::checkFileExtension(std::string("book.Epub"), ".epub"));
  ASSERT_FALSE(StringUtils::checkFileExtension(std::string("book.txt"), ".epub"));
}

void testExtensionShortName() {
  ASSERT_FALSE(StringUtils::checkFileExtension(std::string("a"), ".epub"));
  ASSERT_FALSE(StringUtils::checkFileExtension(std::string(""), ".epub"));
}

// --- checkFileExtension (Arduino String) ---

void testExtensionArduinoString() {
  String fname("book.EPUB");
  ASSERT_TRUE(StringUtils::checkFileExtension(fname, ".epub"));

  String fname2("book.txt");
  ASSERT_FALSE(StringUtils::checkFileExtension(fname2, ".epub"));
}

int main() {
  std::cout << "StringUtilsTest\n";
  RUN_TEST(testSanitizeNormal);
  RUN_TEST(testSanitizeInvalidChars);
  RUN_TEST(testSanitizeTrimSpacesDots);
  RUN_TEST(testSanitizeAllInvalid);
  RUN_TEST(testSanitizeMaxLength);
  RUN_TEST(testSanitizeEmpty);
  RUN_TEST(testSanitizeNonPrintable);
  RUN_TEST(testExtensionEpub);
  RUN_TEST(testExtensionShortName);
  RUN_TEST(testExtensionArduinoString);
  TEST_SUMMARY();
}
