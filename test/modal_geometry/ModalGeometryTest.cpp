#include <gtest/gtest.h>

#include <cstring>

#include "GeometryTarget.h"
#include "src/util/ImageSettingsInput.h"

namespace fui = freeink::ui;

namespace {

constexpr int16_t BODY_X = 16;
constexpr int16_t BODY_W = 368;
constexpr int16_t BODY_Y = 100;
constexpr fui::ActionId ACTION_ROW = imageSettingsInput::kActionRow;

// Production row cadence for a touch target, from the DeviceContext
// primitives a touch device carries (minTouchSize 44 = the SDK default).
constexpr int TOUCH_ROW_H = imageSettingsInput::modalRowHeight(true, 44);
constexpr int16_t ROW_H = static_cast<int16_t>(TOUCH_ROW_H);

// The production DeleteConfirm page fragment around the hazard: two adjacent
// action rows (Cancel / Delete), laid out exactly the way the modal does —
// contiguous bands, list-registered hits, newest-first routing.
void buildDeleteConfirmRows(fui::Frame<16>& frame, const int16_t y) {
  fui::ListItem items[2]{};
  items[0].label = "Cancel";
  items[0].actionValue = 0;
  items[1].label = "Delete";
  items[1].actionValue = 1;

  fui::ListProps props{};
  props.items = items;
  props.count = 2;
  props.scrollIndicator = false;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowHeight = ROW_H;
  fui::list(frame, fui::Rect{BODY_X, y, BODY_W, static_cast<int16_t>(2 * ROW_H)}, props);
}

struct Routed {
  bool routed = false;
  int action = 0;
  int value = 0;
};

// Rebuilds the given page fragment into a fresh frame (the production
// protocol: beginPublishCycle -> build -> publish) and routes one tap.
template <typename PageBuilder>
Routed routeTap(fui::InteractionBuffer<16>& interactions, const freeink::ui::DeviceContext& device, PageBuilder build,
                const int16_t buildY, const int16_t x, const int16_t y) {
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  build(frame, buildY);
  interactions.publish();

  const auto event = interactions.routePublished(tapAt(x, y));
  return {static_cast<bool>(event), event.action, event.value};
}

}  // namespace

// DeleteConfirm destructive invariant, proven against the REAL registration
// and routing (fui::list -> ensureMinTouchRect -> InteractionBuffer):
// every pixel visibly inside the Cancel band routes to Cancel, every pixel
// inside the Delete band routes to Delete, and the bands never overlap — so
// the centered min-touch expansion can never hand a Cancel tap to Delete or
// the reverse (rowHeight == minTouchSize keeps the bands closed).
TEST(ModalGeometry, DeleteConfirmTouchBandsAreDisjointAndRoutedExactly) {
  const auto device = geometryDevice(true, 44);
  fui::InteractionBuffer<16> interactions;
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  buildDeleteConfirmRows(frame, BODY_Y);
  interactions.publish();

  // Exactly the two action rows registered (the SDK's list registers one hit
  // per enabled row; both rows stay in their own band).
  ASSERT_EQ(interactions.publishedCount(), 2u);
  const fui::Interaction* hits = interactions.publishedData();
  const fui::Rect cancelBand = hits[0].rect;
  const fui::Rect deleteBand = hits[1].rect;
  ASSERT_EQ(hits[0].value, 0);
  ASSERT_EQ(hits[1].value, 1);
  // Disjoint by construction: no vertical bleed across the shared edge.
  EXPECT_EQ(cancelBand.bottom(), deleteBand.y);

  // The boundary pixels of each band route to their own row — never across.
  for (int16_t y = cancelBand.y; y < cancelBand.bottom(); ++y) {
    const Routed r = routeTap(interactions, device, buildDeleteConfirmRows, BODY_Y, BODY_X + 10, y);
    ASSERT_TRUE(r.routed) << "y=" << y;
    EXPECT_EQ(r.action, ACTION_ROW) << "y=" << y;
    EXPECT_EQ(r.value, 0) << "y=" << y;   // always Cancel, never Delete
  }
  for (int16_t y = deleteBand.y; y < deleteBand.bottom(); ++y) {
    const Routed r = routeTap(interactions, device, buildDeleteConfirmRows, BODY_Y, BODY_X + 10, y);
    ASSERT_TRUE(r.routed) << "y=" << y;
    EXPECT_EQ(r.action, ACTION_ROW) << "y=" << y;
    EXPECT_EQ(r.value, 1) << "y=" << y;   // always Delete, never Cancel
  }
}

// The stepper fragment of the Image Settings page: one editable row
// (Quantizer, ToneParam index 3) then the Apply row — the hazardous
// adjacency, rendered exactly the way buildSettingsPage() does.
void buildStepperFragment(fui::Frame<16>& frame, const int16_t y) {
  fui::StepperRowProps stepper{};
  stepper.row.label = "Quantizer";
  stepper.row.action = ACTION_ROW;
  stepper.row.valueId = 4;
  stepper.row.inputMask = fui::InputTouch;
  stepper.row.minTouchSize = ROW_H;
  stepper.value = "Default";
  stepper.widestValue = "Canonical";
  stepper.decrement = imageSettingsInput::kActionDecrement;
  stepper.decrementValue = 3;
  stepper.increment = imageSettingsInput::kActionIncrement;
  stepper.incrementValue = 3;
  fui::stepperRow(frame, fui::Rect{BODY_X, y, BODY_W, ROW_H}, stepper);

  fui::SettingRowProps apply{};
  apply.label = "Apply";
  apply.action = ACTION_ROW;
  apply.valueId = 5;
  apply.inputMask = fui::InputTouch;
  apply.minTouchSize = ROW_H;
  fui::settingRow(frame, fui::Rect{BODY_X, static_cast<int16_t>(y + ROW_H), BODY_W, ROW_H}, apply);
}

TEST(ModalGeometry, StepperControlsStayInRowAndRouteToAdjustment) {
  const auto device = geometryDevice(true, 44);
  fui::InteractionBuffer<16> interactions;
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  buildStepperFragment(frame, BODY_Y);
  interactions.publish();

  ASSERT_EQ(interactions.publishedCount(), 4u);
  const fui::Interaction* hits = interactions.publishedData();
  // Registration order: row body, minus, plus, apply.
  const fui::Rect bodyBand = hits[0].rect;
  const fui::Rect minusBand = hits[1].rect;
  const fui::Rect plusBand = hits[2].rect;
  const fui::Rect applyBand = hits[3].rect;
  ASSERT_EQ(hits[0].value, 4);
  ASSERT_EQ(hits[1].action, imageSettingsInput::kActionDecrement);
  ASSERT_EQ(hits[2].action, imageSettingsInput::kActionIncrement);
  ASSERT_EQ(hits[3].value, 5);

  // The +/- control bands stay inside their own row and out of the neighbor
  // (with minTouchSize == row height the stepper's hitPadding fills the band
  // exactly, so ensureMinTouchRect() has nothing left to expand).
  EXPECT_GE(minusBand.y, BODY_Y);
  EXPECT_LE(minusBand.bottom(), BODY_Y + ROW_H);
  EXPECT_GE(plusBand.y, BODY_Y);
  EXPECT_LE(plusBand.bottom(), BODY_Y + ROW_H);
  EXPECT_GE(applyBand.y, BODY_Y + ROW_H);
  EXPECT_EQ(bodyBand.bottom(), applyBand.y);

  // A tap on a control routes to the directional adjustment — never to the
  // row body's Apply route.
  const Routed minus =
      routeTap(interactions, device, buildStepperFragment, BODY_Y, static_cast<int16_t>(minusBand.x + minusBand.width / 2),
               static_cast<int16_t>(minusBand.y + minusBand.height / 2));
  EXPECT_EQ(minus.action, imageSettingsInput::kActionDecrement);
  EXPECT_EQ(minus.value, 3);
  const Routed plus =
      routeTap(interactions, device, buildStepperFragment, BODY_Y, static_cast<int16_t>(plusBand.x + plusBand.width / 2),
               static_cast<int16_t>(plusBand.y + plusBand.height / 2));
  EXPECT_EQ(plus.action, imageSettingsInput::kActionIncrement);
  EXPECT_EQ(plus.value, 3);

  // The label side of the row keeps Confirm parity (Apply route).
  const Routed body = routeTap(interactions, device, buildStepperFragment, BODY_Y, static_cast<int16_t>(bodyBand.x + 10),
                               static_cast<int16_t>(BODY_Y + ROW_H / 2));
  EXPECT_EQ(body.action, ACTION_ROW);
  EXPECT_EQ(body.value, 4);

  // The Apply row's band routes to Apply, never to the stepper above it.
  const Routed applyTap = routeTap(interactions, device, buildStepperFragment, BODY_Y, BODY_X + 10,
                                   static_cast<int16_t>(applyBand.y + 2));
  EXPECT_EQ(applyTap.action, ACTION_ROW);
  EXPECT_EQ(applyTap.value, 5);
}

// Button-only targets resolve the 36px density: the DeviceContext's touch
// minimum is irrelevant when hasTouch is false (the hits are never routed).
TEST(ModalGeometry, RowCadenceResolvesFromDeviceContext) {
  using imageSettingsInput::modalRowHeight;
  EXPECT_EQ(modalRowHeight(true, 0), 36);
  EXPECT_EQ(modalRowHeight(true, 24), 36);
  EXPECT_EQ(modalRowHeight(true, 36), 36);
  EXPECT_EQ(modalRowHeight(true, 44), 44);
  EXPECT_EQ(modalRowHeight(true, 48), 48);
  EXPECT_EQ(modalRowHeight(false, 44), 36);
  EXPECT_EQ(modalRowHeight(false, 24), 36);
}
