#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "FsHelpers.h"

namespace {

using namespace std::string_view_literals;

TEST(ReflowableBookExtension, IncludesTextFormatsAndExcludesImageBooks) {
  for (const auto path : {"/Books/book.epub"sv, "book.TXT"sv, "book.mD"sv}) {
    EXPECT_TRUE(FsHelpers::hasReflowableBookExtension(path));
  }
  for (const auto path : {"book.xtc"sv, "book.XTCH"sv, "book.bmp"sv, "book.epub.bak"sv, ""sv}) {
    EXPECT_FALSE(FsHelpers::hasReflowableBookExtension(path));
  }
}

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

// Post-delete navigation plan (imageIndexAfterRemove, removeOk=true):
// prefer the next image at the same slot, fall back to the previous one,
// -1 = folder becomes empty (exit to the file browser), out-of-range
// deletedIndex is safe.
TEST(ImageIndexAfterRemove, PointsAtNextImageForMiddleDelete) {
  // [a b c d e], delete c(2): the slot now holds d.
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 5, 2), 2);
}

TEST(ImageIndexAfterRemove, FallsBackToPreviousForLastDelete) {
  // [a b c d e], delete e(4): open d at slot 3.
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 5, 4), 3);
}

TEST(ImageIndexAfterRemove, EmptyListMeansExitToBrowser) {
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 1, 0), -1);  // only image
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 0, 0), -1);  // degenerate
}

TEST(ImageIndexAfterRemove, OutOfRangeDeletedIndexIsSafe) {
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 3, 99), 0);
  EXPECT_EQ(FsHelpers::imageIndexAfterRemove(true, 3, -1), 0);
}

// Image Viewer delete contract: a failed physical delete must abort BEFORE
// any list/index mutation — the viewer stays on the current image.
TEST(ImageIndexAfterRemove, FailedRemoveAbortsBeforeStateChange) {
  EXPECT_FALSE(FsHelpers::imageIndexAfterRemove(false, 5, 2).has_value());
  EXPECT_FALSE(FsHelpers::imageIndexAfterRemove(false, 1, 0).has_value());
}

}  // namespace
