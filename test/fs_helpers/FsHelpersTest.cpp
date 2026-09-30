#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "FsHelpers.h"

namespace {

using namespace std::string_view_literals;

TEST(IsSafePathComponent, AcceptsNamesWithRepeatedDots) {
  EXPECT_TRUE(FsHelpers::isSafePathComponent("volume..2.epub"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("notes...txt"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent(".hidden"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("a.b"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("book.epub"sv));
}

TEST(IsSafePathComponent, RejectsEmptyAndExactDotComponents) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent(""sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("."sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent(".."sv));
}

TEST(IsSafePathComponent, RejectsPathSeparatorsAnywhereInTheComponent) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a/b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a\\b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("../x"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("x/.."sv));
}

TEST(NormalisePath, CollapsesParentReferenceWithinPath) {
  EXPECT_EQ(FsHelpers::normalisePath("/Books/../.crosspoint/x"), ".crosspoint/x");
}

TEST(NormalisePath, DropsLeadingParentReferencesPastRoot) { EXPECT_EQ(FsHelpers::normalisePath("/../../etc"), "etc"); }

// Sanitizes into a buffer of `size` bytes (at most 64, the size ScreenshotUtil uses).
std::string sanitize(const char* input, const size_t size = 64) {
  char out[64];
  FsHelpers::sanitizePathComponentForFat32(input, out, size);
  return out;
}

// Book titles from the EPUBs attached to #2103 and #2199.
constexpr char kTitle2103[] = "Богиня глюкозы. Нормализуйте уровень сахара в крови, чтобы изменить свою жизнь";
constexpr char kTitle2199[] = "Вглядываясь в солнце. Жизнь без страха смерти";

TEST(SanitizePathComponentForFat32, KeepsTitleThatFits) {
  EXPECT_EQ(sanitize("Эдем (полный перевод)"), "Эдем-(полный-перевод)");
}

// The readers copy the title into ScreenshotInfo::title (char[64]) with snprintf, which can
// end the copy partway through a Cyrillic letter.
TEST(SanitizePathComponentForFat32, DropsLetterCutOffByCaller) {
  char title[64];
  snprintf(title, sizeof(title), "%s", kTitle2103);
  EXPECT_EQ(sanitize(title), "Богиня-глюкозы.-Нормализуйте-уров");
  snprintf(title, sizeof(title), "%s", kTitle2199);
  EXPECT_EQ(sanitize(title), "Вглядываясь-в-солнце.-Жизнь-без-ст");
}

TEST(SanitizePathComponentForFat32, DoesNotSplitLetterAtBufferLimit) {
  // Each letter of "Жизнь" is 2 bytes. 7 bytes of room holds "Жиз" and half of "н".
  EXPECT_EQ(sanitize("Жизнь", 8), "Жиз");
  EXPECT_EQ(sanitize(kTitle2103), "Богиня-глюкозы.-Нормализуйте-уров");
}

}  // namespace
