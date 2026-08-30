#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/settings/SettingsExtension.h"
#include "activities/settings/SettingsTypes.h"

// SettingsTypes.h only forward-declares Activity, so the test completes it with
// a stand-in that records what a handler did to it. That the header needs no
// more than this is the point: the handler takes the host by reference and
// never needs Activity's rendering dependencies.
//
// startChildForResult()/popChild() reproduce the two halves of the navigation
// contract that matter here: Activity::startActivityForResult() only parks the
// handler and queues the child, and ActivityManager::loop() invokes (and
// clears) that handler when the child pops -- some time after the action
// handler that opened it has returned.
class Activity {
 public:
  int childActivitiesOpened = 0;
  const char* lastChildOpened = nullptr;
  ActivityResultHandler resultHandler;

  void openChild(const char* name) {
    childActivitiesOpened++;
    lastChildOpened = name;
  }

  void startChildForResult(const char* name, ActivityResultHandler handler) {
    openChild(name);
    resultHandler = std::move(handler);
  }

  void popChild(const ActivityResult& result = ActivityResult{}) {
    auto handler = std::move(resultHandler);
    resultHandler = nullptr;
    if (handler) handler(result);
  }
};

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
  category.settings.push_back(
      SettingInfo::ExtensionAction([](Activity&) { toggleState = !toggleState; }).withLabel("Do Thing"));

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
  auto setting = SettingInfo::ExtensionAction([&](Activity&) { callCount++; });

  EXPECT_EQ(setting.type, SettingType::ACTION);
  EXPECT_EQ(setting.action, SettingAction::Extension);
  ASSERT_TRUE(static_cast<bool>(setting.actionHandler));

  Activity host;
  setting.actionHandler(host);
  setting.actionHandler(host);
  EXPECT_EQ(callCount, 2);
}

// The reason the handler takes a host at all: an extension row that navigates.
// Without this an extension could only mutate settings, never contribute a row
// that opens a screen -- which is most of what a real integration wants.
TEST(SettingInfoExtensionAction, HandlerCanOpenAChildActivity) {
  auto setting = SettingInfo::ExtensionAction([](Activity& host) { host.openChild("ChildScreen"); });
  ASSERT_TRUE(static_cast<bool>(setting.actionHandler));

  Activity host;
  setting.actionHandler(host);

  EXPECT_EQ(host.childActivitiesOpened, 1);
  EXPECT_STREQ(host.lastChildOpened, "ChildScreen");
}

// The host is passed by reference, so a handler acts on the activity that
// dispatched it rather than a copy -- SettingsActivity passes *this.
TEST(SettingInfoExtensionAction, HandlerReceivesTheDispatchingHostByReference) {
  Activity* seen = nullptr;
  auto setting = SettingInfo::ExtensionAction([&](Activity& host) { seen = &host; });

  Activity first;
  Activity second;
  setting.actionHandler(first);
  EXPECT_EQ(seen, &first);
  setting.actionHandler(second);
  EXPECT_EQ(seen, &second);
}

TEST(SettingInfoWithLabel, DefaultsEmptyAndOverridesWhenSet) {
  const auto unset = SettingInfo::Toggle(StrId::STR_NONE_OPT, nullptr);
  EXPECT_TRUE(unset.customLabel.empty());

  const auto withLabel = SettingInfo::Toggle(StrId::STR_NONE_OPT, nullptr).withLabel("Custom Row");
  EXPECT_EQ(withLabel.customLabel, "Custom Row");
}

// --- Row initialization: the host compares nameId against specific StrIds to
// decide what activating a row does, so a row that never assigns one must
// still hold a defined, inert value rather than whatever was on the stack. ---

TEST(SettingInfoDefaults, ExtensionActionRowCarriesAnInertNameId) {
  const auto setting = SettingInfo::ExtensionAction([](Activity&) {});

  EXPECT_EQ(setting.nameId, StrId::STR_NONE_OPT);
  // The failure this rules out: an unassigned nameId matching a built-in row
  // the host special-cases, which would route the extension row to the
  // built-in's screen instead of its own handler.
  EXPECT_NE(setting.nameId, StrId::STR_TEST_HOST_SPECIAL_CASED);
}

TEST(SettingInfoDefaults, DefaultConstructedRowIsFullyInitialisedAndInert) {
  // `const` is load-bearing: a class with any member lacking a default member
  // initializer is not const-default-constructible, so this line stops
  // compiling if one is ever dropped again.
  const SettingInfo setting;

  EXPECT_EQ(setting.nameId, StrId::STR_NONE_OPT);
  // ACTION + None dispatches nowhere and mutates nothing.
  EXPECT_EQ(setting.type, SettingType::ACTION);
  EXPECT_EQ(setting.action, SettingAction::None);
  EXPECT_EQ(setting.valuePtr, nullptr);
  EXPECT_EQ(setting.key, nullptr);
  EXPECT_EQ(setting.category, StrId::STR_NONE_OPT);
  EXPECT_FALSE(setting.obfuscated);
  EXPECT_FALSE(setting.inTextSettings);
  EXPECT_EQ(setting.stringOffset, 0u);
  EXPECT_EQ(setting.stringMaxLen, 0u);
  EXPECT_FALSE(static_cast<bool>(setting.actionHandler));
}

TEST(SettingInfoDefaults, ValueRangeStartsZeroed) {
  const SettingInfo::ValueRange range;
  EXPECT_EQ(range.min, 0);
  EXPECT_EQ(range.max, 0);
  EXPECT_EQ(range.step, 0);
}

// --- Tab band range: the provider is re-consulted on every rebuild and may
// return a different number of categories each time, so the band's size is not
// fixed once the screen has been entered. ---

// SettingsActivity::categoryCount.
constexpr int kBuiltInCategories = 4;

TEST(SettingsTabRange, CountCoversTheProvidersCategories) {
  EXPECT_EQ((SettingsTabRange{kBuiltInCategories, 0}.count()), kBuiltInCategories);
  EXPECT_EQ((SettingsTabRange{kBuiltInCategories, 2}.count()), kBuiltInCategories + 2);

  const SettingsTabRange range{kBuiltInCategories, 2};
  EXPECT_FALSE(range.isExtension(kBuiltInCategories - 1));
  EXPECT_TRUE(range.isExtension(kBuiltInCategories));
  EXPECT_EQ(range.extraIndex(kBuiltInCategories), 0);
  EXPECT_EQ(range.extraIndex(kBuiltInCategories + 1), 1);
}

TEST(SettingsTabRange, PerTabStateFollowsAProviderThatAppearsAfterEntry) {
  // Screen entry with no provider registered: per-tab state is sized for the
  // built-ins alone.
  std::vector<int> tabState;
  SettingsTabRange{kBuiltInCategories, 0}.sizeTabState(tabState);
  ASSERT_EQ(tabState.size(), static_cast<size_t>(kBuiltInCategories));
  for (int i = 0; i < kBuiltInCategories; i++) tabState[static_cast<size_t>(i)] = i + 1;

  // A provider then contributes categories. Selecting one of their tabs indexes
  // the per-tab state by tab index, so that state has to grow with the band --
  // sizing it once, before the provider ran, leaves it short by exactly the
  // provider's category count.
  setSettingsExtensionProvider(&SyntheticProvider);
  const SettingsTabRange grown{kBuiltInCategories, static_cast<int>(getSettingsExtensionProvider()().size())};
  grown.sizeTabState(tabState);
  setSettingsExtensionProvider(nullptr);

  ASSERT_EQ(grown.count(), kBuiltInCategories + 2);
  ASSERT_EQ(tabState.size(), static_cast<size_t>(grown.count()));
  // The last selectable tab is addressable in the per-tab state.
  EXPECT_LT(static_cast<size_t>(grown.count() - 1), tabState.size());
  // The tabs that survived keep their state.
  for (int i = 0; i < kBuiltInCategories; i++) EXPECT_EQ(tabState[static_cast<size_t>(i)], i + 1);
}

TEST(SettingsTabRange, PerTabStateAndActiveTabFollowAProviderThatShrinks) {
  std::vector<int> tabState;
  const SettingsTabRange grown{kBuiltInCategories, 2};
  grown.sizeTabState(tabState);
  ASSERT_EQ(tabState.size(), static_cast<size_t>(kBuiltInCategories + 2));

  // Sign-out: the provider drops its categories while one of them is active.
  const SettingsTabRange shrunk{kBuiltInCategories, 0};
  shrunk.sizeTabState(tabState);
  EXPECT_EQ(tabState.size(), static_cast<size_t>(kBuiltInCategories));
  EXPECT_EQ(shrunk.clamp(kBuiltInCategories + 1), kBuiltInCategories - 1);
  EXPECT_EQ(shrunk.clamp(1), 1);
  EXPECT_EQ(shrunk.clamp(-1), 0);
}

// --- Confirm hint: on the tab band Confirm steps to the next tab, so the hint
// has to name the tab that step lands on. ---

TEST(SettingsTabRange, NextTabFromTheLastBuiltInIsTheFirstExtensionTab) {
  const SettingsTabRange range{kBuiltInCategories, 1};
  const int active = kBuiltInCategories - 1;

  // ButtonNavigator::nextIndex(active, tabCount()) -- the wrap
  // SettingsActivity::stepTab() applies, and the one the hint must share.
  const int confirmGoesTo = (active + 1) % range.count();
  // The wrap over the built-in count alone, i.e. as if the provider had
  // contributed nothing: it names a built-in tab Confirm does not go to.
  const int builtInOnlyWrap = (active + 1) % range.builtInCount;

  EXPECT_EQ(confirmGoesTo, kBuiltInCategories);
  EXPECT_TRUE(range.isExtension(confirmGoesTo));
  EXPECT_EQ(range.extraIndex(confirmGoesTo), 0);
  EXPECT_NE(confirmGoesTo, builtInOnlyWrap);
  EXPECT_FALSE(range.isExtension(builtInOnlyWrap));
}

TEST(SettingsTabRange, NextTabFromTheLastExtensionTabWrapsToTheFirstBuiltIn) {
  const SettingsTabRange range{kBuiltInCategories, 2};
  const int active = range.count() - 1;
  EXPECT_EQ((active + 1) % range.count(), 0);
}

// --- Extension action follow-up: startActivityForResult() only queues the
// child screen, so anything the host wants to do with that screen's effects
// has to wait for it to pop. ---

TEST(RunAfterExtensionAction, RunsImmediatelyWhenTheActionOpenedNoScreen) {
  int followUps = 0;
  ActivityResultHandler installed;  // nothing queued

  runAfterExtensionAction(installed, [&] { followUps++; });

  EXPECT_EQ(followUps, 1);
  EXPECT_FALSE(static_cast<bool>(installed));
}

TEST(RunAfterExtensionAction, RunsAfterTheActionsOwnHandlerWhenAScreenWasOpened) {
  std::vector<std::string> order;
  ActivityResultHandler installed = [&](const ActivityResult&) { order.emplace_back("action-handler"); };

  runAfterExtensionAction(installed, [&] { order.emplace_back("follow-up"); });

  // Still nothing: the child has not popped.
  EXPECT_TRUE(order.empty());
  ASSERT_TRUE(static_cast<bool>(installed));

  installed(ActivityResult{});
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "action-handler");
  EXPECT_EQ(order[1], "follow-up");
}

TEST(RunAfterExtensionAction, EmptyFollowUpLeavesTheInstalledHandlerAlone) {
  int handled = 0;
  ActivityResultHandler installed = [&](const ActivityResult&) { handled++; };

  runAfterExtensionAction(installed, nullptr);

  ASSERT_TRUE(static_cast<bool>(installed));
  installed(ActivityResult{});
  EXPECT_EQ(handled, 1);
}

// The failure this closes: a row whose label depends on state its own screen
// changes (a sign-in row that should read differently afterwards) stayed
// stale, because the rebuild ran while the child was still only queued.
TEST(RunAfterExtensionAction, RowsRebuildAgainstTheChildScreensEffects) {
  bool stateTheChildOwns = false;
  int rebuilds = 0;
  std::string rowLabel = "before";
  const auto rebuild = [&] {
    rebuilds++;
    rowLabel = stateTheChildOwns ? "after" : "before";
  };

  auto setting = SettingInfo::ExtensionAction([&](Activity& host) {
    host.startChildForResult("ChildScreen", [&](const ActivityResult&) { stateTheChildOwns = true; });
  });

  Activity host;
  setting.actionHandler(host);
  runAfterExtensionAction(host.resultHandler, rebuild);

  EXPECT_EQ(host.childActivitiesOpened, 1);
  EXPECT_EQ(rebuilds, 0);
  EXPECT_EQ(rowLabel, "before");

  host.popChild();
  EXPECT_EQ(rebuilds, 1);
  EXPECT_EQ(rowLabel, "after");
}

// An action that opens nothing must not be left waiting for a screen that
// never comes.
TEST(RunAfterExtensionAction, RowsRebuildImmediatelyForANonNavigatingAction) {
  int rebuilds = 0;
  auto setting = SettingInfo::ExtensionAction([](Activity&) {});

  Activity host;
  setting.actionHandler(host);
  runAfterExtensionAction(host.resultHandler, [&] { rebuilds++; });

  EXPECT_EQ(host.childActivitiesOpened, 0);
  EXPECT_EQ(rebuilds, 1);
}

}  // namespace
