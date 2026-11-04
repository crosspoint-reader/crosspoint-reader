#include <HalStorage.h>
#include <SdCardFontRegistry.h>
#include <gtest/gtest.h>

#include <string>

namespace {

void addFace(const std::string& filename, const uint16_t weight, const bool italic) {
  const char bytes[] = {static_cast<char>(weight >> 8), static_cast<char>(weight & 255), static_cast<char>(italic)};
  fake::add("/fonts/Family/" + filename, std::string(bytes, sizeof(bytes)));
}

class VectorFontRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  void TearDown() override { fake::reset(); }

  void expectFace(const uint8_t style, const char* filename) {
    const auto* family = registry.findFamily("Family");
    ASSERT_NE(family, nullptr);
    EXPECT_TRUE(family->vector);
    const auto* file = family->findFile(0, style);
    ASSERT_NE(file, nullptr);
    EXPECT_STREQ(file->name.get(), filename);
  }

  SdCardFontRegistry registry;
};

TEST_F(VectorFontRegistryTest, FilenameStylesSurviveMissingItalicMetadata) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-Bold.ttf", 700, false);
  addFace("Family-Italic.ttf", 400, false);
  addFace("Family-BoldItalic.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.findFamily("Family")->files.size(), 4u);
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-Bold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, FilenameStylesWorkWhenInspectionIsUnavailable) {
  for (const char* style : {"Regular", "Bold", "Italic", "BoldItalic"}) {
    fake::add(std::string("/fonts/Family/Family-") + style + ".ttf", "");
  }

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-Bold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, FilenameStylesSurviveRegularWeightAndItalicMetadata) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-Bold.ttf", 400, false);
  addFace("Family-Italic.ttf", 400, false);
  addFace("Family-BoldItalic.ttf", 400, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-Bold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, FilenameBoldHintsWorkWithCorrectItalicMetadataAndAllWeights400) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-Bold.ttf", 400, false);
  addFace("Family-Italic.ttf", 400, true);
  addFace("Family-BoldItalic.ttf", 400, true);

  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Family");
  ASSERT_NE(family, nullptr);
  ASSERT_EQ(family->files.size(), 4u);
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-Bold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, FilenameBoldHintsCoverNonBoldMetadataWeights) {
  for (const uint16_t weight : {0, 300, 400, 500, 599}) {
    SCOPED_TRACE(weight);
    fake::reset();
    addFace("Family-Regular.ttf", 400, false);
    addFace("Family-Bold.ttf", weight, false);
    addFace("Family-Italic.ttf", 400, true);
    addFace("Family-BoldItalic.ttf", weight, true);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "Family-Regular.ttf");
    expectFace(1, "Family-Bold.ttf");
    expectFace(2, "Family-Italic.ttf");
    expectFace(3, "Family-BoldItalic.ttf");
  }
}

TEST_F(VectorFontRegistryTest, BoldMetadataWeightsStillDistinguishSemiboldAndBold) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-ABold.ttf", 600, false);
  addFace("Family-ZBold.ttf", 700, false);
  addFace("Family-Italic.ttf", 400, true);
  addFace("Family-ABoldItalic.ttf", 600, true);
  addFace("Family-ZBoldItalic.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-ZBold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-ZBoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, SemiboldRegularCanPairWithHeavierBold) {
  for (const uint16_t boldWeight : {700, 800, 900}) {
    SCOPED_TRACE(boldWeight);
    fake::reset();
    addFace("Family-Regular.ttf", 600, false);
    addFace("Family-Bold.ttf", boldWeight, false);

    ASSERT_TRUE(registry.discover());
    const auto* family = registry.findFamily("Family");
    ASSERT_NE(family, nullptr);
    ASSERT_EQ(family->files.size(), 2u);
    expectFace(0, "Family-Regular.ttf");
    expectFace(1, "Family-Bold.ttf");
  }
}

TEST_F(VectorFontRegistryTest, MetadataStillSelectsStylesAndNearestWeights) {
  addFace("A.ttf", 400, false);
  addFace("B.ttf", 500, false);
  addFace("C.ttf", 700, false);
  addFace("D.ttf", 400, true);
  addFace("E.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A.ttf");
  expectFace(1, "C.ttf");
  expectFace(2, "D.ttf");
  expectFace(3, "E.ttf");
}

TEST_F(VectorFontRegistryTest, CaseInsensitiveObliqueNamesKeepItalicStyles) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-oBlIqUe.TTF", 400, false);
  addFace("Family-bOlDoBlIqUe.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(2, "Family-oBlIqUe.TTF");
  expectFace(3, "Family-bOlDoBlIqUe.ttf");
  EXPECT_EQ(registry.findFamily("Family")->findFile(0, 1), nullptr);
}

}  // namespace
