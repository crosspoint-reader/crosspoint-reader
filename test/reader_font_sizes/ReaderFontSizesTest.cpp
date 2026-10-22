#include "ReaderFontSizes.h"

#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using Sizes = std::vector<uint8_t>;
const Sizes kBuiltinSizes(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES));

class ReaderFontSizesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Two roots, unsorted on disk; discover() lists families alphabetically.
    fakeSdDirs = {
        {"/.fonts", {{"Beta", true}, {"Alpha", true}, {"readme.txt", false}}},
        {"/.fonts/Beta", {{"Beta_18.cpfont", false}, {"Beta_14.cpfont", false}, {"Beta_16.cpfont.tmp", false}}},
        {"/.fonts/Alpha", {{"Alpha_16.cpfont", false}}},
        {"/fonts", {{"Gamma", true}, {"Empty", true}}},
        {"/fonts/Gamma", {{"Gamma_12.cpfont", false}, {"Gamma_20.cpfont", false}}},
        {"/fonts/Empty", {}},
    };
    registry.discover();
  }

  void TearDown() override { fakeSdDirs.clear(); }

  SdCardFontRegistry registry;
};

TEST(ReaderFontFamilies, NoRegistryListsOnlyBuiltin) {
  EXPECT_EQ(readerFontFamilyCount(nullptr), 1);
  EXPECT_EQ(readerFontFamilyIndex(nullptr, "Beta", 0), 0);
  EXPECT_STREQ(readerFontFamilySdName(nullptr, 0), "");
  EXPECT_EQ(readerFontFamilySdName(nullptr, 1), nullptr);
  EXPECT_EQ(readerFontPointSizes(nullptr, ""), kBuiltinSizes);
}

TEST_F(ReaderFontSizesTest, ListsBuiltinThenSdFamiliesInRegistryOrder) {
  ASSERT_EQ(readerFontFamilyCount(&registry), 4);
  EXPECT_STREQ(readerFontFamilySdName(&registry, 0), "");
  EXPECT_STREQ(readerFontFamilySdName(&registry, 1), "Alpha");
  EXPECT_STREQ(readerFontFamilySdName(&registry, 2), "Beta");
  EXPECT_STREQ(readerFontFamilySdName(&registry, 3), "Gamma");
  EXPECT_EQ(readerFontFamilySdName(&registry, 4), nullptr);
  EXPECT_EQ(readerFontFamilySdName(&registry, -1), nullptr);
}

TEST_F(ReaderFontSizesTest, FindsActiveFamilyIndex) {
  EXPECT_EQ(readerFontFamilyIndex(&registry, "", 0), 0);
  EXPECT_EQ(readerFontFamilyIndex(&registry, "Beta", 0), 2);
  EXPECT_EQ(readerFontFamilyIndex(&registry, "Gamma", 0), 3);
  // A family that is no longer installed, or a bad stored built-in index,
  // shows as the built-in family.
  EXPECT_EQ(readerFontFamilyIndex(&registry, "Removed", 0), 0);
  EXPECT_EQ(readerFontFamilyIndex(&registry, "", 7), 0);
}

TEST_F(ReaderFontSizesTest, OffersOnlySizesTheFamilyShips) {
  EXPECT_EQ(readerFontPointSizes(&registry, readerFontFamilySdName(&registry, 0)), kBuiltinSizes);
  EXPECT_EQ(readerFontPointSizes(&registry, readerFontFamilySdName(&registry, 1)), Sizes({16}));
  EXPECT_EQ(readerFontPointSizes(&registry, readerFontFamilySdName(&registry, 2)), Sizes({14, 18}));
  EXPECT_EQ(readerFontPointSizes(&registry, readerFontFamilySdName(&registry, 3)), Sizes({12, 20}));
  EXPECT_EQ(readerFontPointSizes(&registry, "Removed"), kBuiltinSizes);
}

TEST_F(ReaderFontSizesTest, FamilySwitchKeepsNearestSize) {
  const auto beta = readerFontPointSizes(&registry, "Beta");
  EXPECT_EQ(snapToNearestPointSize(beta, 16), 14);  // tie -> smaller
  EXPECT_EQ(snapToNearestPointSize(beta, 17), 18);
  EXPECT_EQ(snapToNearestPointSize(kBuiltinSizes, 20), 18);
  EXPECT_EQ(snapToNearestPointSize(readerFontPointSizes(&registry, "Gamma"), 14), 12);
}

}  // namespace
