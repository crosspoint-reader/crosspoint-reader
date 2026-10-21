#include <HalStorage.h>
#include <SdCardFontRegistry.h>
#include <gtest/gtest.h>

#include <string>

namespace {

std::string pathOf(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file) {
  char out[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  EXPECT_TRUE(family.filePath(file, out, sizeof(out)));
  return out;
}

class SdFontRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  void TearDown() override { fake::reset(); }
  SdCardFontRegistry registry;
};

TEST_F(SdFontRegistryTest, CanonicalNamesStoreNoFileString) {
  fake::add("/.fonts/Bitter/Bitter_12.cpfont");
  fake::add("/.fonts/Bitter/Bitter_14.cpfont");

  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Bitter");
  ASSERT_NE(family, nullptr);
  EXPECT_STREQ(family->root, SdCardFontRegistry::FONTS_DIR_HIDDEN);
  ASSERT_EQ(family->files.size(), 2u);
  for (const auto& file : family->files) EXPECT_TRUE(file.name.empty());

  const auto* file14 = family->findFile(14);
  ASSERT_NE(file14, nullptr);
  EXPECT_EQ(pathOf(*family, *file14), "/.fonts/Bitter/Bitter_14.cpfont");
  EXPECT_EQ(pathOf(*family, *family->findFile(12)), "/.fonts/Bitter/Bitter_12.cpfont");
}

TEST_F(SdFontRegistryTest, OtherStemsAndZeroPaddedSizesKeepTheirFilename) {
  fake::add("/fonts/Bookerly/Bookerly-SD_14.cpfont");
  fake::add("/fonts/Bookerly/Bookerly_016.cpfont");

  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Bookerly");
  ASSERT_NE(family, nullptr);
  EXPECT_STREQ(family->root, SdCardFontRegistry::FONTS_DIR_VISIBLE);

  const auto* file14 = family->findFile(14);
  ASSERT_NE(file14, nullptr);
  EXPECT_EQ(file14->name, "Bookerly-SD_14.cpfont");
  EXPECT_EQ(pathOf(*family, *file14), "/fonts/Bookerly/Bookerly-SD_14.cpfont");

  const auto* file16 = family->findFile(16);
  ASSERT_NE(file16, nullptr);
  EXPECT_EQ(file16->name, "Bookerly_016.cpfont");
  EXPECT_EQ(pathOf(*family, *file16), "/fonts/Bookerly/Bookerly_016.cpfont");
}

TEST_F(SdFontRegistryTest, HiddenRootWinsAndKeepsItsOwnRoot) {
  fake::add("/.fonts/Lora/Lora_14.cpfont");
  fake::add("/fonts/Lora/Lora_18.cpfont");
  fake::add("/fonts/Inter/Inter_14.cpfont");

  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.getFamilyCount(), 2);
  const auto* lora = registry.findFamily("Lora");
  ASSERT_NE(lora, nullptr);
  EXPECT_STREQ(lora->root, SdCardFontRegistry::FONTS_DIR_HIDDEN);
  EXPECT_FALSE(lora->hasSize(18));
  const auto* inter = registry.findFamily("Inter");
  ASSERT_NE(inter, nullptr);
  EXPECT_EQ(pathOf(*inter, inter->files[0]), "/fonts/Inter/Inter_14.cpfont");
}

TEST_F(SdFontRegistryTest, FilePathReportsTruncation) {
  fake::add("/.fonts/Bitter/Bitter_14.cpfont");
  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Bitter");
  ASSERT_NE(family, nullptr);

  char tiny[16];
  EXPECT_FALSE(family->filePath(family->files[0], tiny, sizeof(tiny)));
  EXPECT_EQ(tiny[sizeof(tiny) - 1], '\0');
}

}  // namespace
