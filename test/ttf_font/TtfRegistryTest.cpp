// How the font registry lists a TrueType family. Directory scanning needs a card; these are the decisions it uses.
#include <gtest/gtest.h>

#include "SdCardFontRegistry.h"

namespace {

SdCardFontFamilyInfo ttfFamily() {
  SdCardFontFamilyInfo family;
  family.name = "Literata";
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Regular.ttf", 0, 0, 450048});
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Bold.ttf", 0, 1, 452000});
  return family;
}

}  // namespace

TEST(TtfRegistry, ATtfFamilyHasNoPerSizeFiles) {
  const SdCardFontFamilyInfo family = ttfFamily();
  EXPECT_TRUE(family.ttf());
  EXPECT_TRUE(family.availableSizes().empty());
  EXPECT_EQ(nullptr, family.pickClosestSize(14));  // previews and the .cpfont loader skip it
  const SdCardFontFileInfo* bold = family.findFile(0, 1);
  ASSERT_NE(nullptr, bold);
  EXPECT_EQ(452000u, bold->fileBytes);
  EXPECT_EQ(nullptr, family.findFile(0, 2));
}

TEST(TtfRegistry, ACpfontFamilyIsUnchanged) {
  SdCardFontFamilyInfo family;
  family.name = "Bookerly-SD";
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_12.cpfont", 12, 0, 1000});
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_16.cpfont", 16, 0, 1200});
  EXPECT_FALSE(family.ttf());
  EXPECT_EQ((std::vector<uint8_t>{12, 16}), family.availableSizes());
  ASSERT_NE(nullptr, family.pickClosestSize(15));
  EXPECT_EQ(16, family.pickClosestSize(15)->pointSize);
}

TEST(TtfRegistry, AFamilyRecordCostsOnlyItsNameAndFiles) {
  // The registry reserves MAX_SD_FAMILIES (128) records at every discovery and keeps them: a field here is
  // 128 times permanent heap, so whether a family is TTF is derived from its files instead.
  EXPECT_EQ(sizeof(std::string) + sizeof(std::vector<SdCardFontFileInfo>), sizeof(SdCardFontFamilyInfo));
}
