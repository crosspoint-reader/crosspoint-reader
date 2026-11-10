#include <gtest/gtest.h>

#include <array>
#include <string>

#include "CrossPointSettings.h"
#include "HalStorage.h"
#include "PanelCurve.h"

class PanelCurveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage = {};
    SETTINGS = {};
    panelcurve::forget();
  }
  static std::string curve(const int value) {
    std::string result;
    for (int i = 0; i < 256; ++i) result += std::to_string(value) + "\n";
    return result;
  }
  std::array<uint8_t, 256> pictures(uint32_t* outHash = nullptr) {
    std::array<uint8_t, 256> result{};
    uint32_t hash;
    EXPECT_TRUE(panelcurve::load(result.data(), hash));
    if (outHash) *outHash = hash;
    return result;
  }
  void expectConstant(const uint8_t* table, const int value) {
    for (int i = 0; i < 256; ++i) EXPECT_EQ(table[i], value) << i;
  }
};

TEST_F(PanelCurveTest, HiddenPictureCurveWinsOverLegacyRootFile) {
  Storage.files[panelcurve::PATH] = curve(61);
  Storage.files[panelcurve::ROOT_PATH] = curve(24);
  expectConstant(pictures().data(), 61);
  EXPECT_EQ(Storage.files.at(panelcurve::ROOT_PATH), curve(24));
  EXPECT_EQ(Storage.files.at(panelcurve::PATH), curve(61));
}

TEST_F(PanelCurveTest, LegacyCurvesRemainReadableWithoutChangingTheCard) {
  Storage.files[panelcurve::ROOT_PATH] = curve(45);
  Storage.files[panelcurve::BOOK_ROOT_PATH] = curve(83);
  const auto before = Storage.files;
  expectConstant(pictures().data(), 45);
  uint32_t hash;
  expectConstant(panelcurve::bookCurve(hash), 83);
  EXPECT_EQ(Storage.files, before);
}

TEST_F(PanelCurveTest, InvalidLegacyDataUsesTheIdentityCurveWithoutChangingTheCard) {
  const auto identity = pictures();
  Storage.files[panelcurve::ROOT_PATH] = "0 255";
  panelcurve::forget();
  EXPECT_EQ(pictures(), identity);
  EXPECT_EQ(Storage.files.at(panelcurve::ROOT_PATH), "0 255");
  EXPECT_FALSE(Storage.exists(panelcurve::PATH));
}

TEST_F(PanelCurveTest, UnreadableLegacyDataUsesTheIdentityCurve) {
  const auto identity = pictures();
  Storage.files[panelcurve::ROOT_PATH] = curve(45);
  Storage.canRead = false;
  panelcurve::forget();
  EXPECT_EQ(pictures(), identity);
  EXPECT_TRUE(Storage.exists(panelcurve::ROOT_PATH));
  EXPECT_FALSE(Storage.exists(panelcurve::PATH));
}

TEST_F(PanelCurveTest, InvalidHiddenConfigurationDoesNotLoadALegacyCurve) {
  const auto identity = pictures();
  Storage.files[panelcurve::PATH] = "0 255";
  Storage.files[panelcurve::ROOT_PATH] = curve(45);
  panelcurve::forget();
  EXPECT_EQ(pictures(), identity);
  EXPECT_EQ(Storage.files.at(panelcurve::PATH), "0 255");
  EXPECT_TRUE(Storage.exists(panelcurve::ROOT_PATH));
}

TEST_F(PanelCurveTest, ForgetReloadsTheHiddenCurve) {
  Storage.files[panelcurve::PATH] = curve(45);
  expectConstant(pictures().data(), 45);
  Storage.files[panelcurve::PATH] = curve(68);
  expectConstant(pictures().data(), 45);
  panelcurve::forget();
  expectConstant(pictures().data(), 68);
}

TEST_F(PanelCurveTest, HiddenBookCurveWinsIndependentlyOfThePictureCurve) {
  Storage.files[panelcurve::PATH] = curve(45);
  Storage.files[panelcurve::BOOK_PATH] = curve(68);
  Storage.files[panelcurve::BOOK_ROOT_PATH] = curve(83);
  expectConstant(pictures().data(), 45);
  uint32_t hash;
  expectConstant(panelcurve::bookCurve(hash), 68);
  EXPECT_EQ(Storage.files.at(panelcurve::BOOK_ROOT_PATH), curve(83));
}

TEST_F(PanelCurveTest, DisabledCalibrationKeepsFilesAndUsesIdentity) {
  SETTINGS.panelCalPictures = false;
  SETTINGS.panelCalBooks = false;
  Storage.files[panelcurve::ROOT_PATH] = curve(45);
  Storage.files[panelcurve::BOOK_ROOT_PATH] = curve(83);
  const auto identity = pictures();
  for (int i = 0; i < 256; ++i) EXPECT_EQ(identity[i], i);
  uint32_t hash;
  EXPECT_EQ(panelcurve::bookCurve(hash), nullptr);
  EXPECT_TRUE(Storage.exists(panelcurve::ROOT_PATH));
  EXPECT_TRUE(Storage.exists(panelcurve::BOOK_ROOT_PATH));
}

TEST_F(PanelCurveTest, MissingFilesUseIdentityForBothChannels) {
  const auto table = pictures();
  uint32_t hash;
  const auto* book = panelcurve::bookCurve(hash);
  ASSERT_NE(book, nullptr);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(table[i], i);
    EXPECT_EQ(book[i], i);
  }
}

TEST_F(PanelCurveTest, RejectsSignedValuesAndExtraFinalValueWithoutASeparator) {
  for (const auto& invalid : {"-1 " + curve(45).substr(3), curve(45) + "1", "x " + curve(45)}) {
    Storage.files[panelcurve::PATH] = invalid;
    panelcurve::forget();
    const auto table = pictures();
    for (int i = 0; i < 256; ++i) EXPECT_EQ(table[i], i);
  }
}
