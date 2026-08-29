#include <gtest/gtest.h>

#include "KeyboardExtension.h"

// KeyboardExtension.h only forward-declares this, so the test completes it.
// Nothing in the extension point dereferences a layout -- it stores and returns
// pointers -- which is exactly what makes the hook host-testable at all.
namespace freeink {
namespace ui {
struct KeyboardLayout {
  int id = 0;
};
}  // namespace ui
}  // namespace freeink

namespace {

freeink::ui::KeyboardLayout kBase{1};
freeink::ui::KeyboardLayout kShifted{2};

int gLayerCalls = 0;
bool gLastShifted = false;

const freeink::ui::KeyboardLayout* bothStatesProvider(const bool shifted) {
  gLayerCalls++;
  gLastShifted = shifted;
  return shifted ? &kShifted : &kBase;
}

// Declines when shifted -- a provider offering an unshifted layer only.
const freeink::ui::KeyboardLayout* unshiftedOnlyProvider(const bool shifted) { return shifted ? nullptr : &kBase; }

bool alwaysPrefer() { return true; }
bool neverPrefer() { return false; }

class KeyboardExtensionTest : public ::testing::Test {
 protected:
  void SetUp() override { reset(); }
  void TearDown() override { reset(); }

  static void reset() {
    setKeyboardExtensionLayerProvider(nullptr);
    setKeyboardExtensionDefaultPredicate(nullptr);
    setKeyboardExtensionLabelProvider(nullptr);
    gLayerCalls = 0;
    gLastShifted = false;
  }
};

// The property that matters most: a stock build installs nothing, so both
// hooks read back null and every call site collapses to a null check.
TEST_F(KeyboardExtensionTest, DefaultsToNoProvider) {
  EXPECT_EQ(getKeyboardExtensionLayerProvider(), nullptr);
  EXPECT_EQ(getKeyboardExtensionDefaultPredicate(), nullptr);
}

TEST_F(KeyboardExtensionTest, LayerProviderRoundTrips) {
  setKeyboardExtensionLayerProvider(&bothStatesProvider);
  ASSERT_NE(getKeyboardExtensionLayerProvider(), nullptr);
  EXPECT_EQ(getKeyboardExtensionLayerProvider(), &bothStatesProvider);
}

TEST_F(KeyboardExtensionTest, LayerProviderReceivesShiftStateAndReturnsPerStateLayouts) {
  setKeyboardExtensionLayerProvider(&bothStatesProvider);
  const auto provider = getKeyboardExtensionLayerProvider();
  ASSERT_NE(provider, nullptr);

  EXPECT_EQ(provider(false), &kBase);
  EXPECT_FALSE(gLastShifted);
  EXPECT_EQ(provider(true), &kShifted);
  EXPECT_TRUE(gLastShifted);
  EXPECT_EQ(gLayerCalls, 2);
}

// A provider may decline a state; the activity treats nullptr as "no extra
// layer available" and falls back to the built-in layers.
TEST_F(KeyboardExtensionTest, ProviderMayDeclineAState) {
  setKeyboardExtensionLayerProvider(&unshiftedOnlyProvider);
  const auto provider = getKeyboardExtensionLayerProvider();
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(provider(false), &kBase);
  EXPECT_EQ(provider(true), nullptr);
}

TEST_F(KeyboardExtensionTest, DefaultPredicateRoundTripsAndIsIndependent) {
  // Independent of the layer provider: a build may offer a layer without
  // opening on it, which is the expected default.
  setKeyboardExtensionLayerProvider(&bothStatesProvider);
  EXPECT_EQ(getKeyboardExtensionDefaultPredicate(), nullptr);

  setKeyboardExtensionDefaultPredicate(&alwaysPrefer);
  ASSERT_NE(getKeyboardExtensionDefaultPredicate(), nullptr);
  EXPECT_TRUE(getKeyboardExtensionDefaultPredicate()());

  setKeyboardExtensionDefaultPredicate(&neverPrefer);
  EXPECT_FALSE(getKeyboardExtensionDefaultPredicate()());
}

TEST_F(KeyboardExtensionTest, ProvidersCanBeClearedBackToStock) {
  setKeyboardExtensionLayerProvider(&bothStatesProvider);
  setKeyboardExtensionDefaultPredicate(&alwaysPrefer);
  ASSERT_NE(getKeyboardExtensionLayerProvider(), nullptr);
  ASSERT_NE(getKeyboardExtensionDefaultPredicate(), nullptr);

  setKeyboardExtensionLayerProvider(nullptr);
  setKeyboardExtensionDefaultPredicate(nullptr);
  EXPECT_EQ(getKeyboardExtensionLayerProvider(), nullptr);
  EXPECT_EQ(getKeyboardExtensionDefaultPredicate(), nullptr);
}

// A default predicate with no layer provider must stay inert: the activity
// checks layer availability before consulting it, so nothing can select a
// layer that does not exist.
TEST_F(KeyboardExtensionTest, DefaultPredicateAloneOffersNoLayer) {
  setKeyboardExtensionDefaultPredicate(&alwaysPrefer);
  EXPECT_EQ(getKeyboardExtensionLayerProvider(), nullptr);
  EXPECT_TRUE(getKeyboardExtensionDefaultPredicate()());
}

const char* kLayerName = "\xd8\xb9\xd8\xb1";  // arbitrary non-ASCII: CrossPoint only draws it

const char* labelProvider() { return kLayerName; }

TEST_F(KeyboardExtensionTest, LabelProviderRoundTripsAndIsOpaque) {
  EXPECT_EQ(getKeyboardExtensionLabelProvider(), nullptr);
  setKeyboardExtensionLabelProvider(&labelProvider);
  ASSERT_NE(getKeyboardExtensionLabelProvider(), nullptr);
  EXPECT_STREQ(getKeyboardExtensionLabelProvider()(), kLayerName);
  setKeyboardExtensionLabelProvider(nullptr);
  EXPECT_EQ(getKeyboardExtensionLabelProvider(), nullptr);
}

// All three hooks are independent: installing a layer does not imply a label,
// and the activity falls back to the layout table's own label when unset.
TEST_F(KeyboardExtensionTest, LayerWithoutLabelIsSupported) {
  setKeyboardExtensionLayerProvider(&bothStatesProvider);
  ASSERT_NE(getKeyboardExtensionLayerProvider(), nullptr);
  EXPECT_EQ(getKeyboardExtensionLabelProvider(), nullptr);
}

}  // namespace
