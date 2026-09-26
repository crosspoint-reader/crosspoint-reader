#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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

void expectNaturalOrder(std::vector<std::string> names, const std::vector<std::string>& expected) {
  std::sort(names.begin(), names.end(), FsHelpers::naturalLess);
  EXPECT_EQ(names, expected);
}

TEST(NaturalLess, OrdersNumericComponentsBeforeFileExtensions) {
  expectNaturalOrder({"Book 10.epub", "Book 2.5.epub", "Book 1.5.epub", "Book 2.epub", "Book 1.epub"},
                     {"Book 1.epub", "Book 1.5.epub", "Book 2.epub", "Book 2.5.epub", "Book 10.epub"});
  expectNaturalOrder({"Book 6.epub", "Book 5.5.epub", "Book 5.epub"}, {"Book 5.epub", "Book 5.5.epub", "Book 6.epub"});
}

TEST(NaturalLess, ComparesComponentsRatherThanDecimalValues) {
  expectNaturalOrder({"Bible 2.1", "Bible 1.10", "Bible 1.2"}, {"Bible 1.2", "Bible 1.10", "Bible 2.1"});
  expectNaturalOrder({"v2.0", "v1.10", "v1.2"}, {"v1.2", "v1.10", "v2.0"});
  expectNaturalOrder({"1.10", "1.2.10", "1.2.3", "1.2"}, {"1.2", "1.2.3", "1.2.10", "1.10"});
  expectNaturalOrder({"10", "2", "1"}, {"1", "2", "10"});
}

TEST(NaturalLess, BreaksNumericTiesByOriginalSpelling) {
  expectNaturalOrder({"0001", "01", "001", "1"}, {"1", "01", "001", "0001"});
  expectNaturalOrder({"1.005", "1.5", "1.05"}, {"1.5", "1.05", "1.005"});
  EXPECT_NE(FsHelpers::naturalLess("Book 1.epub", "Book 01.epub"),
            FsHelpers::naturalLess("Book 01.epub", "Book 1.epub"));
}

TEST(NaturalLess, HasStrictWeakOrderingOnRepresentativeNames) {
  const std::vector<std::string> names = {
      "",          "1",          "01",   "001",   "0001", "1.", "1..2", "1.0",         "1.2",           "1.2.3",
      "1.2.10",    "1.10",       "1a",   "a",     "A",    "a1", "a01",  "Book 1.epub", "Book 1.5.epub", "Book 2.epub",
      "Bible 1.2", "Bible 1.10", "v1.2", "v1.10", "v2.0"};
  const auto less = FsHelpers::naturalLess;
  for (const auto& a : names) {
    EXPECT_FALSE(less(a, a)) << a;
    for (const auto& b : names) {
      if (less(a, b)) EXPECT_FALSE(less(b, a)) << a << " / " << b;
      for (const auto& c : names) {
        if (less(a, b) && less(b, c)) EXPECT_TRUE(less(a, c)) << a << " / " << b << " / " << c;
        if (!less(a, b) && !less(b, a) && !less(b, c) && !less(c, b)) {
          EXPECT_FALSE(less(a, c)) << a << " / " << b << " / " << c;
          EXPECT_FALSE(less(c, a)) << a << " / " << b << " / " << c;
        }
      }
    }
  }
}

TEST(SortFileList, KeepsDirectoriesFirst) {
  std::vector<std::string> names = {"Book 5.5.epub", "Folder 10/", "Book 5.epub", "Folder 2/"};
  FsHelpers::sortFileList(names);
  EXPECT_EQ(names, (std::vector<std::string>{"Folder 2/", "Folder 10/", "Book 5.epub", "Book 5.5.epub"}));
}

}  // namespace
