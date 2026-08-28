#include <gtest/gtest.h>

#include <vector>

#include "activities/settings/SettingsExtension.h"
#include "activities/settings/SettingsTypes.h"

namespace {

// The provider is process-global state; every test starts and ends with it
// unset so tests can't leak into each other regardless of run order.
class SettingsExtensionTest : public ::testing::Test {
 protected:
  void SetUp() override { setSettingsExtensionProvider(nullptr); }
  void TearDown() override { setSettingsExtensionProvider(nullptr); }
};

std::vector<SettingsExtensionCategory> EmptyProvider() { return {}; }

std::vector<SettingsExtensionCategory> SyntheticProvider() {
  static bool toggleState = false;
  static uint8_t valueState = 0;

  SettingsExtensionCategory category;
  category.label = "Synthetic";
  category.settings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_TEST_ROW_A, [] { return static_cast<uint8_t>(toggleState); },
      [](uint8_t v) { toggleState = v != 0; }));
  category.settings.push_back(SettingInfo::DynamicValue(
      StrId::STR_TEST_ROW_B, [] { return valueState; }, [](uint8_t v) { valueState = v; },
      SettingInfo::ValueRange{0, 10, 5}));
  category.settings.push_back(SettingInfo::ExtensionAction([] { toggleState = !toggleState; }).withLabel("Do Thing"));

  SettingsExtensionCategory second;
  second.label = "Second";
  return {category, second};
}

// --- Provider storage: proves the "unset = default CrossPoint behavior,
// zero extra branching" contract at the API level. ---

TEST_F(SettingsExtensionTest, DefaultsToNoProvider) { EXPECT_EQ(getSettingsExtensionProvider(), nullptr); }

TEST_F(SettingsExtensionTest, SetProviderRoundTrips) {
  setSettingsExtensionProvider(&EmptyProvider);
  EXPECT_EQ(getSettingsExtensionProvider(), &EmptyProvider);
}

TEST_F(SettingsExtensionTest, ClearingProviderRestoresDefault) {
  setSettingsExtensionProvider(&EmptyProvider);
  setSettingsExtensionProvider(nullptr);
  EXPECT_EQ(getSettingsExtensionProvider(), nullptr);
}

// --- Synthetic provider: proves extra categories/rows/values/toggles/
// actions all round-trip through the real SettingInfo factories. ---

TEST_F(SettingsExtensionTest, SyntheticProviderContributesExpectedCategoriesAndRows) {
  setSettingsExtensionProvider(&SyntheticProvider);
  const auto categories = getSettingsExtensionProvider()();

  ASSERT_EQ(categories.size(), 2u);
  EXPECT_EQ(categories[0].label, "Synthetic");
  EXPECT_EQ(categories[1].label, "Second");
  ASSERT_EQ(categories[0].settings.size(), 3u);

  const auto& toggleRow = categories[0].settings[0];
  EXPECT_EQ(toggleRow.type, SettingType::TOGGLE);
  ASSERT_TRUE(static_cast<bool>(toggleRow.valueGetter));
  ASSERT_TRUE(static_cast<bool>(toggleRow.valueSetter));

  const auto& valueRow = categories[0].settings[1];
  EXPECT_EQ(valueRow.type, SettingType::VALUE);
  EXPECT_EQ(valueRow.valueRange.min, 0);
  EXPECT_EQ(valueRow.valueRange.max, 10);
  EXPECT_EQ(valueRow.valueRange.step, 5);

  const auto& actionRow = categories[0].settings[2];
  EXPECT_EQ(actionRow.type, SettingType::ACTION);
  EXPECT_EQ(actionRow.action, SettingAction::Extension);
  EXPECT_EQ(actionRow.customLabel, "Do Thing");
}

// --- Mutation semantics: same one-line contract SettingsActivity::
// toggleCurrentSetting() applies for a dynamically-backed TOGGLE/VALUE row
// (setting.valueSetter(!setting.valueGetter()) / wrap-on-overflow step). ---

TEST(SettingInfoDynamicToggle, GetterSetterRoundTrip) {
  bool state = false;
  const auto setting = SettingInfo::DynamicToggle(
      StrId::STR_TEST_ROW_A, [&] { return static_cast<uint8_t>(state); }, [&](uint8_t v) { state = v != 0; });

  EXPECT_EQ(setting.type, SettingType::TOGGLE);
  EXPECT_EQ(setting.valueGetter(), 0);

  setting.valueSetter(!setting.valueGetter());
  EXPECT_TRUE(state);

  setting.valueSetter(!setting.valueGetter());
  EXPECT_FALSE(state);
}

TEST(SettingInfoDynamicValue, StepsAndWrapsOnOverflow) {
  uint8_t state = 8;
  const auto setting = SettingInfo::DynamicValue(
      StrId::STR_TEST_ROW_B, [&] { return state; }, [&](uint8_t v) { state = v; }, SettingInfo::ValueRange{0, 10, 5});

  // 8 + 5 = 13 > max(10) -> wraps to min, matching
  // SettingsActivity::toggleCurrentSetting()'s VALUE-with-valuePtr branch.
  const uint8_t current = setting.valueGetter();
  if (current + setting.valueRange.step > setting.valueRange.max) {
    setting.valueSetter(setting.valueRange.min);
  } else {
    setting.valueSetter(current + setting.valueRange.step);
  }
  EXPECT_EQ(state, 0);
}

TEST(SettingInfoExtensionAction, DispatchesToHandler) {
  int callCount = 0;
  auto setting = SettingInfo::ExtensionAction([&] { callCount++; });

  EXPECT_EQ(setting.type, SettingType::ACTION);
  EXPECT_EQ(setting.action, SettingAction::Extension);
  ASSERT_TRUE(static_cast<bool>(setting.actionHandler));

  setting.actionHandler();
  setting.actionHandler();
  EXPECT_EQ(callCount, 2);
}

TEST(SettingInfoWithLabel, DefaultsEmptyAndOverridesWhenSet) {
  const auto unset = SettingInfo::Toggle(StrId::STR_NONE_OPT, nullptr);
  EXPECT_TRUE(unset.customLabel.empty());

  const auto withLabel = SettingInfo::Toggle(StrId::STR_NONE_OPT, nullptr).withLabel("Custom Row");
  EXPECT_EQ(withLabel.customLabel, "Custom Row");
}

}  // namespace
