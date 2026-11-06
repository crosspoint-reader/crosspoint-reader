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

TEST_F(VectorFontRegistryTest, ExplicitDuplicateRolesPreferTargetWeightOverFilenameOrder) {
  addFace("Family-Regular.ttf", 400, false);
  addFace("A-Bold.ttf", 600, false);
  addFace("Z-Bold.ttf", 700, false);
  addFace("Family-Italic.ttf", 400, true);
  addFace("A-BoldItalic.ttf", 600, true);
  addFace("Z-BoldItalic.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Family-Regular.ttf");
  expectFace(1, "Z-Bold.ttf");
  expectFace(2, "Family-Italic.ttf");
  expectFace(3, "Z-BoldItalic.ttf");
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
  } cases[] = {{"Family-Regular.ttf", true},     {"Family-regular.ttf", true},       {"Family Regular.ttf", true},
               {"Family_Regular.ttf", true},     {"FamilyRegular.ttf", false},       {"FamilyREGULAR.TTF", false},
               {"Regular.otf", false},           {"Family-Roman.ttc", true},         {"Familyregular.ttf", false},
               {"regular.ttf", false},           {"Family-RegularExtra.ttf", false}, {"Family-Regular.more.ttf", false},
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
    const std::string filename = std::string("Family-") + suffix + ".ttf";
    addFace(filename, 400, false);
    addFace("Control-Regular.ttf", 400, false);

    ASSERT_TRUE(registry.discover());
    expectFace(3, filename.c_str());
    EXPECT_EQ(registry.findFamily("Family")->findFile(0, 2), nullptr);
  }
}

TEST_F(VectorFontRegistryTest, SeparatedCombinedSuffixesAcceptMixedSeparators) {
  static constexpr struct {
    const char* left;
    const char* right;
  } cases[] = {
      {"Bold", "Italic"}, {"Bold", "Oblique"}, {"Black", "Italic"}, {"Black", "Oblique"}, {"bLaCk", "oBlIqUe"}};
  for (const auto& test : cases) {
    for (const char outer : {' ', '-', '_'}) {
      for (const char inner : {' ', '-', '_'}) {
        const std::string filename = std::string("Family") + outer + test.left + inner + test.right + ".ttf";
        SCOPED_TRACE(filename);
        fake::reset();
        addFace("Control-Regular.ttf", 400, false);
        addFace(filename, 400, false);

        ASSERT_TRUE(registry.discover());
        expectFace(3, filename.c_str());
        EXPECT_EQ(registry.findFamily("Family")->findFile(0, 2), nullptr);
      }
    }
  }
}

TEST_F(VectorFontRegistryTest, InvalidCollapsedSuffixKeepsOrdinaryTrailingSuffix) {
  for (const char* filename : {"Family-Something-Italic.ttf", "Family Something-Oblique.ttf", "FamilyBold-Italic.ttf",
                               "Bold-Italic.ttf", "Family-Bold--Italic.ttf"}) {
    SCOPED_TRACE(filename);
    fake::reset();
    addFace("Control-Regular.ttf", 400, false);
    addFace(filename, 700, true);

    ASSERT_TRUE(registry.discover());
    expectFace(2, filename);
    EXPECT_EQ(registry.findFamily("Family")->findFile(0, 3), nullptr);
  }
}

TEST_F(VectorFontRegistryTest, ConcatenatedBoldIsOnlyAHintNotAnExplicitRole) {
  for (const char* filename : {"FamilyBold.ttf", "Familybold.ttf", "FamilyBOLD.ttf"}) {
    SCOPED_TRACE(filename);
    fake::reset();
    addFace("Control-Regular.ttf", 400, false);
    addFace(filename, 400, true);
    addFace("Z.ttf", 700, false);

    ASSERT_TRUE(registry.discover());
    expectFace(1, "Z.ttf");
    expectFace(3, filename);
  }
}

TEST_F(VectorFontRegistryTest, SemiboldCapitalizationDoesNotReserveBoldRole) {
  for (const char* filename : {"FamilySemiBold.ttf", "FamilySemibold.ttf", "FamilySEMIBOLD.ttf"}) {
    SCOPED_TRACE(filename);
    fake::reset();
    addFace(filename, 500, false);
    addFace("Other.ttf", 700, false);

    ASSERT_TRUE(registry.discover());
    expectFace(0, filename);
    expectFace(1, "Other.ttf");
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

TEST_F(VectorFontRegistryTest, FallbackPrefersTargetWeightsOverThinAndHeavyExtremes) {
  addFace("A.ttf", 300, false);
  addFace("Z.ttf", 400, false);
  addFace("B.ttf", 900, false);
  addFace("Y.ttf", 700, false);
  addFace("C.ttf", 300, true);
  addFace("X.ttf", 400, true);
  addFace("D.ttf", 900, true);
  addFace("W.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Z.ttf");
  expectFace(1, "Y.ttf");
  expectFace(2, "X.ttf");
  expectFace(3, "W.ttf");
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

TEST_F(VectorFontRegistryTest, ExactBoldTargetIsPreferredOverSemiboldForBoldRoles) {
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

TEST_F(VectorFontRegistryTest, ExactBoldTargetWinsEvenWhenCombinedSemiboldFilenameSortsFirst) {
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

TEST_F(VectorFontRegistryTest, FilenameBoldHintStillUsesMetadataWeightForRanking) {
  addFace("Control-Regular.ttf", 400, false);
  addFace("A-demiboldish.ttf", 400, false);
  addFace("Z.ttf", 700, false);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Control-Regular.ttf");
  expectFace(1, "Z.ttf");
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

TEST_F(VectorFontRegistryTest, RegularTargets400AndFallbackItalicMatchesItsWeight) {
  for (const uint16_t lowWeight : {300, 400, 401, 500, 599}) {
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

TEST_F(VectorFontRegistryTest, WeightCloserTo400StillWinsWithFilenameBoldHints) {
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

TEST_F(VectorFontRegistryTest, PromotionRefillAllowsIntermediateWeightForItalic) {
  addFace("A-Italic.ttf", 400, true);
  addFace("A-bolditalicized.ttf", 600, true);
  addFace("Z.ttf", 700, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "A-Italic.ttf");
  expectFace(2, "A-bolditalicized.ttf");
  expectFace(3, "Z.ttf");
}

TEST_F(VectorFontRegistryTest, NonBoldRankingUsesDistanceThenHeavierWeightThenFilename) {
  static constexpr struct {
    uint16_t first;
    uint16_t second;
    bool secondWins;
  } cases[] = {{300, 500, true},  {310, 500, false}, {300, 490, true},
               {400, 500, false}, {500, 500, false}, {100, 600, true}};
  for (const bool suffixes : {false, true}) {
    for (const auto& test : cases) {
      SCOPED_TRACE(suffixes);
      SCOPED_TRACE(test.first);
      SCOPED_TRACE(test.second);
      fake::reset();
      const char* firstRegular = suffixes ? "A-Regular.ttf" : "A.ttf";
      const char* secondRegular = suffixes ? "Z-Regular.ttf" : "Z.ttf";
      const char* firstItalic = suffixes ? "B-Italic.ttf" : "B.ttf";
      const char* secondItalic = suffixes ? "Y-Italic.ttf" : "Y.ttf";
      addFace(firstRegular, test.first, false);
      addFace(secondRegular, test.second, false);
      addFace(firstItalic, test.first, true);
      addFace(secondItalic, test.second, true);

      ASSERT_TRUE(registry.discover());
      expectFace(0, test.secondWins ? secondRegular : firstRegular);
      expectFace(2, test.secondWins ? secondItalic : firstItalic);
    }
  }
}

TEST_F(VectorFontRegistryTest, BoldRankingUsesRegularPlus300ThenHeavierWeightThenFilename) {
  static constexpr struct {
    uint16_t first;
    uint16_t second;
    bool secondWins;
  } cases[] = {{800, 1000, true}, {810, 1000, false}, {800, 990, true}, {900, 1000, false}, {800, 800, false}};
  for (const bool suffixes : {false, true}) {
    for (const auto& test : cases) {
      SCOPED_TRACE(suffixes);
      SCOPED_TRACE(test.first);
      SCOPED_TRACE(test.second);
      fake::reset();
      addFace("Control-Regular.ttf", 600, false);
      addFace("Control-Italic.ttf", 400, true);
      const char* firstBold = suffixes ? "A-Bold.ttf" : "A.ttf";
      const char* secondBold = suffixes ? "Z-Bold.ttf" : "Z.ttf";
      const char* firstBoldItalic = suffixes ? "B-BoldItalic.ttf" : "B.ttf";
      const char* secondBoldItalic = suffixes ? "Y-BoldItalic.ttf" : "Y.ttf";
      addFace(firstBold, test.first, false);
      addFace(secondBold, test.second, false);
      addFace(firstBoldItalic, test.first, true);
      addFace(secondBoldItalic, test.second, true);

      ASSERT_TRUE(registry.discover());
      expectFace(1, test.secondWins ? secondBold : firstBold);
      expectFace(3, test.secondWins ? secondBoldItalic : firstBoldItalic);
    }
  }
}

TEST_F(VectorFontRegistryTest, ThinRegularSetsBoldTargetBelow700ForSuffixAndFallbackCandidates) {
  for (const bool suffixes : {false, true}) {
    SCOPED_TRACE(suffixes);
    fake::reset();
    addFace("Control-Regular.ttf", 250, false);
    addFace("Control-Italic.ttf", 250, true);
    const char* lighterBold = suffixes ? "A-Bold.ttf" : "A.ttf";
    const char* heavierBold = suffixes ? "B-Bold.ttf" : "B.ttf";
    const char* preferredBold = suffixes ? "Z-Bold.ttf" : "Z.ttf";
    const char* lighterBoldItalic = suffixes ? "C-BoldItalic.ttf" : "C.ttf";
    const char* heavierBoldItalic = suffixes ? "D-BoldItalic.ttf" : "D.ttf";
    const char* preferredBoldItalic = suffixes ? "Y-BoldItalic.ttf" : "Y.ttf";
    addFace(lighterBold, 500, false);
    addFace(heavierBold, 700, false);
    addFace(preferredBold, 600, false);
    addFace(lighterBoldItalic, 500, true);
    addFace(heavierBoldItalic, 700, true);
    addFace(preferredBoldItalic, 600, true);

    ASSERT_TRUE(registry.discover());
    expectFace(0, "Control-Regular.ttf");
    expectFace(1, preferredBold);
    expectFace(2, "Control-Italic.ttf");
    expectFace(3, preferredBoldItalic);
    EXPECT_EQ(registry.findFamily("Family")->files.size(), 4u);
  }
}

TEST_F(VectorFontRegistryTest, CloserSemiboldBeatsHeavierBoldForBothBoldRoles) {
  addFace("Control-Regular.ttf", 400, false);
  addFace("Control-Italic.ttf", 400, true);
  addFace("A.ttf", 900, false);
  addFace("B.ttf", 900, true);
  addFace("Y-boldish.ttf", 600, false);
  addFace("Z-bolditalicized.ttf", 600, true);

  ASSERT_TRUE(registry.discover());
  expectFace(1, "Y-boldish.ttf");
  expectFace(3, "Z-bolditalicized.ttf");
}

TEST_F(VectorFontRegistryTest, FallbackRegularSetsTargetForExplicitBoldDuplicates) {
  addFace("RegularCandidate.ttf", 600, false);
  addFace("A-Bold.ttf", 700, false);
  addFace("Z-Bold.ttf", 900, false);
  addFace("A-BoldItalic.ttf", 700, true);
  addFace("Z-BoldItalic.ttf", 900, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "RegularCandidate.ttf");
  expectFace(1, "Z-Bold.ttf");
  expectFace(3, "Z-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, PromotedRegularSetsTargetForBothBoldRoles) {
  addFace("Only-Italic.ttf", 600, true);
  addFace("A-Bold.ttf", 700, false);
  addFace("Z-Bold.ttf", 900, false);
  addFace("A-BoldItalic.ttf", 700, true);
  addFace("Z-BoldItalic.ttf", 900, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Italic.ttf");
  expectFace(1, "Z-Bold.ttf");
  expectFace(3, "Z-BoldItalic.ttf");
}

TEST_F(VectorFontRegistryTest, BoldPromotionTargets700ThenRefillUsesPromotedWeightPlus300) {
  for (const char* suffix : {"Bold", "BoldItalic"}) {
    SCOPED_TRACE(suffix);
    fake::reset();
    const std::string first = std::string("A-") + suffix + ".ttf";
    const std::string closest = std::string("Y-") + suffix + ".ttf";
    const std::string refill = std::string("Z-") + suffix + ".ttf";
    addFace(first, 900, false);
    addFace(closest, 700, false);
    addFace(refill, 1000, false);

    ASSERT_TRUE(registry.discover());
    expectFace(0, closest.c_str());
    expectFace(std::string(suffix) == "Bold" ? 1 : 3, refill.c_str());
    EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
  }
}

TEST_F(VectorFontRegistryTest, BoldTargetDoesNotWrapAtMaximumMetadataWeight) {
  addFace("Control-Regular.ttf", 65535, false);
  addFace("A-Bold.ttf", 200, false);
  addFace("Z-Bold.ttf", 65534, false);

  ASSERT_TRUE(registry.discover());
  expectFace(1, "Z-Bold.ttf");
}

TEST_F(VectorFontRegistryTest, FallbackItalicMatchesRegularWeightThenHeavierThenFilename) {
  static constexpr struct {
    uint16_t regular;
    uint16_t first;
    uint16_t second;
    bool secondWins;
  } cases[] = {{100, 400, 100, true},  {300, 200, 400, true},  {300, 210, 400, false}, {300, 200, 390, true},
               {300, 300, 400, false}, {300, 350, 350, false}, {600, 400, 600, true}};
  for (const bool metadataItalic : {false, true}) {
    for (const auto& test : cases) {
      SCOPED_TRACE(metadataItalic);
      SCOPED_TRACE(test.regular);
      SCOPED_TRACE(test.first);
      SCOPED_TRACE(test.second);
      fake::reset();
      addFace("Control-Regular.ttf", test.regular, false);
      const char* first = metadataItalic ? "A.ttf" : "A-italicized.ttf";
      const char* second = metadataItalic ? "Z.ttf" : "Z-italicized.ttf";
      addFace(first, test.first, metadataItalic);
      addFace(second, test.second, metadataItalic);

      ASSERT_TRUE(registry.discover());
      expectFace(0, "Control-Regular.ttf");
      expectFace(2, test.secondWins ? second : first);
    }
  }
}

TEST_F(VectorFontRegistryTest, FallbackItalicUsesFallbackSelectedRegularWeight) {
  addFace("RegularCandidate.ttf", 100, false);
  addFace("A.ttf", 400, true);
  addFace("Z.ttf", 100, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "RegularCandidate.ttf");
  expectFace(2, "Z.ttf");
}

TEST_F(VectorFontRegistryTest, ExplicitItalicDuplicatesStillTarget400RatherThanRegularWeight) {
  addFace("Control-Regular.ttf", 100, false);
  addFace("A-Italic.ttf", 100, true);
  addFace("Z-Oblique.ttf", 400, true);
  addFace("Fallback.ttf", 100, true);

  ASSERT_TRUE(registry.discover());
  expectFace(2, "Z-Oblique.ttf");
}

TEST_F(VectorFontRegistryTest, ExplicitItalicStillWinsOverCloserFallbackWeight) {
  addFace("Control-Regular.ttf", 100, false);
  addFace("Explicit-Italic.ttf", 400, true);
  addFace("Fallback.ttf", 100, true);

  ASSERT_TRUE(registry.discover());
  expectFace(2, "Explicit-Italic.ttf");
}

TEST_F(VectorFontRegistryTest, PromotedRegularWeightSetsFallbackItalicRefillTarget) {
  addFace("Only-Italic.ttf", 100, true);
  addFace("A.ttf", 400, true);
  addFace("Z.ttf", 120, true);

  ASSERT_TRUE(registry.discover());
  expectFace(0, "Only-Italic.ttf");
  expectFace(2, "Z.ttf");
  EXPECT_EQ(registry.findFamily("Family")->files.size(), 2u);
}

}  // namespace
