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

TEST_F(VectorFontRegistryTest, ExplicitStylesAreRejectedWhenInspectionIsUnavailable) {
  for (const char* style : {"Regular", "Bold", "Italic", "BoldItalic"}) {
    fake::add(std::string("/fonts/Family/Family-") + style + ".ttf", "");
  }

  EXPECT_FALSE(registry.discover());
  EXPECT_EQ(registry.findFamily("Family"), nullptr);
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

TEST_F(VectorFontRegistryTest, ExplicitDuplicateRolesUseFilenameOrderNotWeight) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("Family-ABold.ttf", 600, false);
  addFace("Family-ZBold.ttf", 700, false);
  addFace("Family-Italic.ttf", 400, true);
  addFace("Family-ABoldItalic.ttf", 600, true);
  addFace("Family-ZBoldItalic.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Family-ABold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Family-ABoldItalic.ttf");
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

TEST_F(VectorFontRegistryTest, MetadataSelectsExactStyleFlags) {
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

TEST_F(VectorFontRegistryTest, AllSuffixAliasesOverrideConflictingMetadata) {
  static constexpr struct {
    const char* suffix;
    uint8_t role;
  } cases[] = {{"Regular", 0},     {"Book", 0},    {"Roman", 0},      {"Bold", 1},        {"Black", 1},
               {"Italic", 2},      {"Oblique", 2}, {"BoldItalic", 3}, {"BoldOblique", 3}, {"BlackOblique", 3},
               {"BlackItalic", 3}, {"rEgUlAr", 0}, {"bLaCkItAlIc", 3}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.suffix);
    fake::reset();
    const std::string filename = std::string("Family-") + test.suffix + ".ttf";
    addFace(filename, (test.role & 1) ? 400 : 900, (test.role & 2) == 0);
    if (test.role != 0) addFace("Control-Regular.ttf", 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(test.role, filename.c_str());
    EXPECT_EQ(registry.findFamily("Family")->files.size(), test.role == 0 ? 1u : 2u);
    EXPECT_EQ(fake::reads, test.role == 0 ? 1u : 2u);
  }
}

TEST_F(VectorFontRegistryTest, SuffixesRespectStartAndEndBoundaries) {
  static constexpr struct {
    const char* filename;
    bool explicitRegular;
  } cases[] = {{"Family-Regular.ttf", true},
               {"Family-regular.ttf", true},
               {"Family Regular.ttf", true},
               {"Family_Regular.ttf", true},
               {"FamilyRegular.ttf", true},
               {"FamilyREGULAR.TTF", true},
               {"Regular.otf", true},
               {"Family-Roman.ttc", true},
               {"Familyregular.ttf", false},
               {"regular.ttf", false},
               {"Family-RegularExtra.ttf", false},
               {"Family-Regular.more.ttf", false},
               {"Family-regularized.ttf", false}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.filename);
    fake::reset();
    addFace(test.filename, 700, true);
    if (!test.explicitRegular) addFace("Control-Regular.ttf", 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(test.explicitRegular ? 0 : 3, test.filename);
    EXPECT_EQ(registry.findFamily("Family")->files.size(), test.explicitRegular ? 1u : 2u);
  }
}

TEST_F(VectorFontRegistryTest, CombinedSuffixesPrecedeItalicTails) {
  for (const char* suffix : {"BoldItalic", "BoldOblique", "BlackItalic", "BlackOblique"}) {
    SCOPED_TRACE(suffix);
    fake::reset();
    const std::string filename = std::string("Family") + suffix + ".ttf";
    addFace(filename, 400, false);
    addFace("Control-Regular.ttf", 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(3, filename.c_str());
    EXPECT_EQ(registry.findFamily("Family")->findFile(0, 2), nullptr);
  }
}

TEST_F(VectorFontRegistryTest, ExplicitRolesWinButEveryCandidateIsInspected) {
  addFace("Z-Regular.ttf", 900, true);
  addFace("Z-Bold.ttf", 400, true);
  addFace("Z-Italic.ttf", 900, false);
  addFace("Z-BoldItalic.ttf", 400, false);
  addFace("A.ttf", 400, false);
  addFace("B.ttf", 700, false);
  addFace("C.ttf", 400, true);
  addFace("D.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z-Regular.ttf");
  expectFace(1, "Z-Bold.ttf");
  expectFace(2, "Z-Italic.ttf");
  expectFace(3, "Z-BoldItalic.ttf");
  EXPECT_EQ(fake::reads, 8u);
}

TEST_F(VectorFontRegistryTest, DuplicateExplicitFilesAreNotReusedForOtherRoles) {
  addFace("A-Regular.ttf", 700, true);
  addFace("Z-Regular.ttf", 700, false);
  addFace("Fallback.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A-Regular.ttf");
  expectFace(1, "Fallback.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
  EXPECT_EQ(fake::reads, 3u);
}

TEST_F(VectorFontRegistryTest, FallbackUsesLexicalOrderRatherThanNearestWeight) {
  addFace("A.ttf", 300, false);
  addFace("Z.ttf", 400, false);
  addFace("B.ttf", 900, false);
  addFace("Y.ttf", 700, false);
  addFace("C.ttf", 300, true);
  addFace("X.ttf", 400, true);
  addFace("D.ttf", 900, true);
  addFace("W.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A.ttf");
  expectFace(1, "B.ttf");
  expectFace(2, "C.ttf");
  expectFace(3, "D.ttf");
}

TEST_F(VectorFontRegistryTest, FallbackOrsEachFilenameHintWithMetadata) {
  addFace("RegularCandidate.ttf", 400, false);
  addFace("semiboldish.ttf", 400, false);
  addFace("obliquish.ttf", 400, true);
  addFace("italicized.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "RegularCandidate.ttf");
  expectFace(1, "semiboldish.ttf");
  expectFace(2, "obliquish.ttf");
  expectFace(3, "italicized.ttf");
}

TEST_F(VectorFontRegistryTest, FallbackCombinesFilenameBoldWithMetadataItalic) {
  addFace("semiboldish.ttf", 400, true);
  addFace("Control-Regular.ttf", 400, false);

  ASSERT_TRUE(registry.discover());
  expectFace(3, "semiboldish.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
}

TEST_F(VectorFontRegistryTest, UnavailableMetadataRejectsUnsuffixedCandidates) {
  for (const char* filename : {"plain.ttf", "semiboldish.ttf", "italicized.ttf", "bolditalicized.ttf"}) {
    fake::add(std::string("/fonts/Family/") + filename, "");
  }

  EXPECT_FALSE(registry.discover());
  EXPECT_EQ(registry.findFamily("Family"), nullptr);
}

TEST_F(VectorFontRegistryTest, InvalidLexicalFirstSuffixDoesNotHideValidDuplicate) {
  fake::add("/fonts/Family/A-Regular.ttf", "invalid");
  fake::add("/fonts/Family/A-Bold.ttf", "");
  addFace("Z-Regular.ttf", 400, false);
  addFace("Z-Bold.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z-Regular.ttf");
  expectFace(1, "Z-Bold.ttf");
}

TEST_F(VectorFontRegistryTest, InvalidUnsuffixedCandidateDoesNotHideValidCandidate) {
  fake::add("/fonts/Family/A.ttf", "invalid");
  addFace("Z.ttf", 400, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 1u);
}

TEST_F(VectorFontRegistryTest, FailedCandidateCanReturnOnLaterDiscovery) {
  fake::add("/fonts/Family/A-Regular.ttf", "");
  addFace("Z-Regular.ttf", 400, false);
  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z-Regular.ttf");
  addFace("A-Regular.ttf", 400, false);
  ASSERT_TRUE(registry.discover());
  expectFace(0, "A-Regular.ttf");
}

TEST_F(VectorFontRegistryTest, ReadFailureDoesNotReserveAnExplicitRole) {
  addFace("A-Regular.ttf", 400, false);
  addFace("Z-Regular.ttf", 400, false);
  fake::failRead = 0;

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z-Regular.ttf");
}

TEST_F(VectorFontRegistryTest, AliasesAreNotAddedToLegacyHints) {
  addFace("blackish.ttf", 400, false);
  addFace("Z.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "blackish.ttf");
  expectFace(1, "Z.ttf");
}

TEST_F(VectorFontRegistryTest, OneItalicFileIsPromotedWithoutBeingAssignedTwice) {
  addFace("Only.ttf", 400, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 1u);
  EXPECT_EQ(registry.findFamily("Family")->findFile(0, 2), nullptr);
}

TEST_F(VectorFontRegistryTest, ARegularRoleIsAlwaysPickedForEachSingleFileStyle) {
  for (const char* suffix : {"Regular", "Bold", "Italic", "BoldItalic"}) {
    SCOPED_TRACE(suffix);
    fake::reset();
    const std::string filename = std::string("Only-") + suffix + ".ttf";
    addFace(filename, 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(0, filename.c_str());
    EXPECT_EQ(registry.findFamily("Family")->files.size(), 1u);
  }
}

TEST_F(VectorFontRegistryTest, PromotionPrefersItalicThenBoldThenBoldItalic) {
  addFace("Only-Italic.ttf", 400, true);
  addFace("Only-Bold.ttf", 700, false);
  addFace("Only-BoldItalic.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Italic.ttf");
  expectFace(1, "Only-Bold.ttf");
  expectFace(3, "Only-BoldItalic.ttf");
  EXPECT_EQ(registry.findFamily("Family")->findFile(0, 2), nullptr);

  fake::reset();
  addFace("Only-Bold.ttf", 700, false);
  addFace("Only-BoldItalic.ttf", 700, true);
  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Bold.ttf");
  expectFace(3, "Only-BoldItalic.ttf");
  EXPECT_EQ(registry.findFamily("Family")->findFile(0, 1), nullptr);
}

TEST_F(VectorFontRegistryTest, PromotedRoleIsRefilledFromUnusedExplicitFilesFirst) {
  addFace("A-Italic.ttf", 700, false);
  addFace("Z-Italic.ttf", 700, false);
  addFace("A.ttf", 400, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A-Italic.ttf");
  expectFace(2, "Z-Italic.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
}

TEST_F(VectorFontRegistryTest, EachPromotedStyleCanBeRefilledFromItsDuplicateSuffix) {
  static constexpr struct {
    const char* suffix;
    uint8_t role;
  } cases[] = {{"Italic", 2}, {"Bold", 1}, {"BoldItalic", 3}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.suffix);
    fake::reset();
    const std::string first = std::string("A-") + test.suffix + ".ttf";
    const std::string second = std::string("Z-") + test.suffix + ".ttf";
    addFace(first, 400, false);
    addFace(second, 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(0, first.c_str());
    expectFace(test.role, second.c_str());
    EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
  }
}

TEST_F(VectorFontRegistryTest, PromotedRoleIsRefilledFromUnusedFallbackFilesInLexicalOrder) {
  addFace("Only-Italic.ttf", 700, false);
  addFace("A.ttf", 400, true);
  addFace("Z.ttf", 400, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Italic.ttf");
  expectFace(2, "A.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
}

TEST_F(VectorFontRegistryTest, FallbackOnlyPromotionAlsoRefillsTheVacatedRole) {
  addFace("A.ttf", 400, true);
  addFace("B.ttf", 400, true);
  addFace("C.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A.ttf");
  expectFace(1, "C.ttf");
  expectFace(2, "B.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 3u);
}

TEST_F(VectorFontRegistryTest, AnExistingRegularRoleIsNotReplacedByPromotion) {
  addFace("Only-Regular.ttf", 700, true);
  addFace("Only-Italic.ttf", 400, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Regular.ttf");
  expectFace(2, "Only-Italic.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
}

TEST_F(VectorFontRegistryTest, NormalWeightIsPreferredOverSemiboldForNonBoldRoles) {
  for (const uint16_t weight : {401, 500, 600, 699}) {
    SCOPED_TRACE(weight);
    fake::reset();
    addFace("A.ttf", weight, false);
    addFace("B.ttf", weight, true);
    addFace("Y.ttf", 400, false);
    addFace("Z.ttf", 400, true);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "Y.ttf");
    expectFace(1, "A.ttf");
    expectFace(2, "Z.ttf");
    expectFace(3, "B.ttf");
  }
}

TEST_F(VectorFontRegistryTest, SemiboldSuppliesNonBoldRolesBeforeRegularPromotion) {
  for (const uint16_t weight : {401, 500, 600, 699}) {
    SCOPED_TRACE(weight);
    fake::reset();
    addFace("A.ttf", 700, false);
    addFace("B.ttf", 700, true);
    addFace("Y.ttf", weight, false);
    addFace("Z.ttf", weight, true);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "Y.ttf");
    expectFace(1, "A.ttf");
    expectFace(2, "Z.ttf");
    expectFace(3, "B.ttf");
  }
}

TEST_F(VectorFontRegistryTest, PureBoldIsPreferredOverSemiboldForBoldRoles) {
  for (const uint16_t weight : {401, 500, 600, 699}) {
    SCOPED_TRACE(weight);
    fake::reset();
    addFace("Control-Regular.ttf", 400, false);
    addFace("Control-Italic.ttf", 400, true);
    addFace("A.ttf", weight, false);
    addFace("B.ttf", weight, true);
    addFace("Y.ttf", 700, false);
    addFace("Z.ttf", 700, true);

    ASSERT_TRUE(registry.discover());
    expectFace(1, "Y.ttf");
    expectFace(3, "Z.ttf");
  }
}

TEST_F(VectorFontRegistryTest, SemiboldMetadataDisambiguatesCombinedFilenameBoldHints) {
  addFace("A.ttf", 700, false);
  addFace("B.ttf", 700, true);
  addFace("semiboldish.ttf", 600, false);
  addFace("semibolditalicized.ttf", 600, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "semiboldish.ttf");
  expectFace(1, "A.ttf");
  expectFace(2, "semibolditalicized.ttf");
  expectFace(3, "B.ttf");
}

TEST_F(VectorFontRegistryTest, PureBoldWinsEvenWhenCombinedSemiboldFilenameSortsFirst) {
  addFace("Control-Regular.ttf", 400, false);
  addFace("Control-Italic.ttf", 400, true);
  addFace("A-boldish.ttf", 600, false);
  addFace("B-bolditalicized.ttf", 600, true);
  addFace("Y.ttf", 700, false);
  addFace("Z.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(1, "Y.ttf");
  expectFace(3, "Z.ttf");
}

TEST_F(VectorFontRegistryTest, SemiboldHintIsNeverDerivedFromFilenameAlone) {
  addFace("Control-Regular.ttf", 400, false);
  addFace("A-demiboldish.ttf", 400, false);
  addFace("Z.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Control-Regular.ttf");
  expectFace(1, "A-demiboldish.ttf");
}

TEST_F(VectorFontRegistryTest, MetadataWeight400DoesNotSupplyBoldBut401Does) {
  for (const uint16_t weight : {400, 401}) {
    SCOPED_TRACE(weight);
    fake::reset();
    addFace("Control-Regular.ttf", 400, false);
    addFace("Only.ttf", weight, false);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "Control-Regular.ttf");
    if (weight == 400) {
      EXPECT_EQ(registry.findFamily("Family")->findFile(0, 1), nullptr);
    } else {
      expectFace(1, "Only.ttf");
    }
  }
}

TEST_F(VectorFontRegistryTest, LexicalOrderStillBreaksTiesWithinSemiboldClass) {
  addFace("A.ttf", 500, false);
  addFace("B.ttf", 500, false);
  addFace("C.ttf", 500, true);
  addFace("D.ttf", 500, true);
  addFace("Y.ttf", 700, false);
  addFace("Z.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A.ttf");
  expectFace(1, "Y.ttf");
  expectFace(2, "C.ttf");
  expectFace(3, "Z.ttf");
}

TEST_F(VectorFontRegistryTest, LowestMetadataWeightWinsNonBoldRoles) {
  for (const uint16_t lowWeight : {100, 300, 400, 401, 500, 599}) {
    SCOPED_TRACE(lowWeight);
    fake::reset();
    addFace("A.ttf", 600, false);
    addFace("B.ttf", lowWeight, false);
    addFace("C.ttf", 600, true);
    addFace("D.ttf", lowWeight, true);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "B.ttf");
    expectFace(1, "A.ttf");
    expectFace(2, "D.ttf");
    expectFace(3, "C.ttf");
  }
}

TEST_F(VectorFontRegistryTest, LowestWeightStillWinsWithFilenameBoldHints) {
  addFace("A-boldish.ttf", 600, false);
  addFace("B-boldish.ttf", 500, false);
  addFace("C-bolditalicized.ttf", 600, true);
  addFace("D-bolditalicized.ttf", 500, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "B-boldish.ttf");
  expectFace(1, "A-boldish.ttf");
  expectFace(2, "D-bolditalicized.ttf");
  expectFace(3, "C-bolditalicized.ttf");
}

TEST_F(VectorFontRegistryTest, PromotionRefillUsesSemiboldPreferenceForVacatedNonBoldRole) {
  addFace("A-Italic.ttf", 400, true);
  addFace("A-bolditalicized.ttf", 600, true);
  addFace("Z.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A-Italic.ttf");
  expectFace(2, "A-bolditalicized.ttf");
  expectFace(3, "Z.ttf");
}

}  // namespace
