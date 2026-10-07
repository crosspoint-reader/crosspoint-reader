#include <gtest/gtest.h>

#include <cstdint>

#include "src/util/ImageSettingsInput.h"

// The step policy clamps into the profile layer's min/max constraints
// (ToneLut.h) — the same composition the production activity uses.
#include "lib/GfxRenderer/ToneLut.h"

// The Image Settings page is 2-axis: one button event -> exactly ONE action.
// Vertical axis (row selection): side Up/Down ONLY. Horizontal axis (value
// stepping): front Left/Right ONLY. No merged NavNext/NavPrevious semantics.
TEST(ImageSettingsInput, VerticalAxisOnlyMovesRows) {
  using imageSettingsInput::Action;
  using imageSettingsInput::Button;
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Up, true, false), Action::RowUp);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Down, true, false), Action::RowDown);
  // Release edges of the vertical keys are NOT actions (no double-fire).
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Up, false, true), Action::None);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Down, false, true), Action::None);
}

TEST(ImageSettingsInput, HorizontalAxisOnlyStepsValues) {
  using imageSettingsInput::Action;
  using imageSettingsInput::Button;
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Left, true, false), Action::ValueDown);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Right, true, false), Action::ValueUp);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Left, false, true), Action::None);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Right, false, true), Action::None);
}

TEST(ImageSettingsInput, ActionButtonsOnReleaseOnly) {
  using imageSettingsInput::Action;
  using imageSettingsInput::Button;
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Confirm, false, true), Action::Activate);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Back, false, true), Action::Back);
  // A press-only edge (button going down, still held) never acts.
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Confirm, true, false), Action::None);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Back, true, false), Action::None);
}

TEST(ImageSettingsInput, NoEventMeansNoAction) {
  using imageSettingsInput::Action;
  using imageSettingsInput::Button;
  for (int b = 0; b <= 6; ++b) {
    EXPECT_EQ(imageSettingsInput::actionFor(static_cast<Button>(b), false, false), Action::None);
  }
}

TEST(ImageSettingsInput, PressWinsOverSimultaneousReleaseWindow) {
  using imageSettingsInput::Action;
  using imageSettingsInput::Button;
  // A directional button observed with both edges in one dispatch window still
  // maps to a single action (the press edge).
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Left, true, true), Action::ValueDown);
  // Confirm/Back DO fire on a both-edges window: a capacitive Home-key tap
  // (Confirm) and a back swipe are ONE event that MappedInputManager reports
  // on both wasPressed and wasReleased (MappedInputManager.cpp wasPressed/
  // wasReleased both fold homeAction/wasBackGesture in). BmpViewerActivity
  // routes Confirm/Back through this mapping, so the touch-adjacent event
  // paths must survive it.
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Confirm, true, true), Action::Activate);
  EXPECT_EQ(imageSettingsInput::actionFor(Button::Back, true, true), Action::Back);
}

// Confirm-activation routing over the page's selectable rows.
TEST(ImageSettingsInput, ConfirmRoutesSettingsRows) {
  using imageSettingsInput::confirmActionForRow;
  using imageSettingsInput::RowAction;
  // Row order: 0 = Reset to defaults (restages the draft ONLY), 1..4 = the
  // editable B/G/C/Q rows, 5 = the explicit Apply row, 6 = Use for sleep
  // rendering. Editable rows and Apply share the one viewer-Apply route
  // (Confirm applies without navigating down); the sleep row is the ONLY
  // save-profile route — never viewer Apply.
  EXPECT_EQ(confirmActionForRow(0, 4), RowAction::ResetDraft);
  for (int row = 1; row <= 4; ++row) {
    EXPECT_EQ(confirmActionForRow(row, 4), RowAction::ApplyViewer) << "row=" << row;
  }
  EXPECT_EQ(confirmActionForRow(5, 4), RowAction::ApplyViewer);
  EXPECT_EQ(confirmActionForRow(6, 4), RowAction::SaveSleepProfile);
  // Out-of-range rows activate nothing; routes are mutually exclusive by
  // construction (an enum return can carry only one action).
  EXPECT_EQ(confirmActionForRow(-1, 4), RowAction::None);
  EXPECT_EQ(confirmActionForRow(7, 4), RowAction::None);
}

TEST(ImageSettingsInput, ResetDraftIsDistinctFromApply) {
  // Reset to defaults restages the draft only: it is its own action, distinct
  // from the viewer-Apply and save-profile routes, so Confirm on the Reset row
  // can never also persist, apply or render.
  using imageSettingsInput::RowAction;
  EXPECT_NE(RowAction::ResetDraft, RowAction::ApplyViewer);
  EXPECT_NE(RowAction::ResetDraft, RowAction::SaveSleepProfile);
}

TEST(ImageSettingsInput, EditableParamMappingSkipsActionRows) {
  using imageSettingsInput::editableParamForRow;
  // Reset / Apply / sleep rows (and anything out of range) step nothing on
  // Left/Right; value rows map to the ToneParam order shifted by the Reset row.
  EXPECT_EQ(editableParamForRow(-1, 4), -1);
  EXPECT_EQ(editableParamForRow(0, 4), -1);
  EXPECT_EQ(editableParamForRow(5, 4), -1);
  EXPECT_EQ(editableParamForRow(6, 4), -1);
  EXPECT_EQ(editableParamForRow(7, 4), -1);
  for (int row = 1; row <= 4; ++row) {
    EXPECT_EQ(editableParamForRow(row, 4), row - 1) << "row=" << row;
  }
}

// The Left/Right step policy lives in the UI layer (not ToneLut.h): one step
// moves a percent-encoded value by TONE_STEP, clamped to the parameter range.
TEST(ImageSettingsInput, StepClampsAtRangeBounds) {
  using imageSettingsInput::stepClamped;
  using imageSettingsInput::TONE_STEP;
  // Brightness 70..110 step 5: cannot exceed the bounds in either direction.
  EXPECT_EQ(TONE_STEP, 5);
  EXPECT_EQ(stepClamped(110, 1, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX), 110);
  EXPECT_EQ(stepClamped(70, -1, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX), 70);
  EXPECT_EQ(stepClamped(105, 1, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX), 110);
  // Gamma (x100) 70..130, Contrast 80..130 use the same helper.
  EXPECT_EQ(stepClamped(130, 1, TONE_GAMMA_MIN, TONE_GAMMA_MAX), 130);
  EXPECT_EQ(stepClamped(80, -1, TONE_CONTRAST_MIN, TONE_CONTRAST_MAX), 80);
  EXPECT_EQ(stepClamped(100, 1, TONE_CONTRAST_MIN, TONE_CONTRAST_MAX), 105);
}

// Image Settings are offered only for formats whose decode path consumes the
// tone pipeline (BMP via Bitmap + ToneLut). PNG (PngToFramebufferConverter)
// has no tone hook: offering the page there would let a user persist B/G/C/Q
// settings that never affect the image.
TEST(ImageSettingsInput, ImageSettingsOnlyForToneCapableFormats) {
  using imageSettingsInput::imageSettingsAvailable;
  EXPECT_TRUE(imageSettingsAvailable(true));    // BMP: Bitmap + ToneLut apply
  EXPECT_FALSE(imageSettingsAvailable(false));  // PNG: decode path is tone-free
}

// Quantizer presentation must be derived from the persisted value alone:
// -1 = no override ("Default"), 0 = explicit Legacy, 1 = explicit Canonical.
// A -1 override must NOT read as "Legacy" — on a controller whose fallback is
// canonical that would be wrong.
TEST(ImageSettingsInput, QuantizerLabelIsTriState) {
  using imageSettingsInput::QuantizerLabel;
  using imageSettingsInput::quantizerLabel;
  EXPECT_EQ(quantizerLabel(-1), QuantizerLabel::Default);
  EXPECT_EQ(quantizerLabel(0), QuantizerLabel::Legacy);
  EXPECT_EQ(quantizerLabel(1), QuantizerLabel::Canonical);
  // The three labels are distinct: the UI can never show one state as another.
  EXPECT_NE(QuantizerLabel::Default, QuantizerLabel::Legacy);
  EXPECT_NE(QuantizerLabel::Default, QuantizerLabel::Canonical);
  EXPECT_NE(QuantizerLabel::Legacy, QuantizerLabel::Canonical);
}

TEST(ImageSettingsInput, QuantizerSteppedIsADirectionalCycle) {
  using imageSettingsInput::quantizerStepped;
  // ONE cyclic space walked in both directions (persisted domain unchanged:
  // -1 = Default, 0 = Legacy, 1 = Canonical). The two visually different
  // stepper controls move opposite ways, and the hardware Right key still
  // lands on Canonical first from the Default, as it always did:
  //   +1: Default -> Canonical -> Legacy -> Default
  //   -1: Default -> Legacy -> Canonical -> Default
  EXPECT_EQ(quantizerStepped(-1, 1), 1);   // Default +  -> Canonical
  EXPECT_EQ(quantizerStepped(1, 1), 0);    // Canonical + -> Legacy
  EXPECT_EQ(quantizerStepped(0, 1), -1);   // Legacy +    -> Default
  EXPECT_EQ(quantizerStepped(-1, -1), 0);  // Default -   -> Legacy
  EXPECT_EQ(quantizerStepped(0, -1), 1);   // Legacy -    -> Canonical
  EXPECT_EQ(quantizerStepped(1, -1), -1);  // Canonical - -> Default
  // Full cycles in both directions return to the starting state.
  int8_t v = -1;
  for (int i = 0; i < 3; ++i) v = quantizerStepped(v, 1);
  EXPECT_EQ(v, -1);
  v = -1;
  for (int i = 0; i < 3; ++i) v = quantizerStepped(v, -1);
  EXPECT_EQ(v, -1);
  // Non-UI values act as Default (the load path normalizes them away).
  EXPECT_EQ(quantizerStepped(42, 1), 1);
  EXPECT_EQ(quantizerStepped(42, -1), 0);
}

// Touch ActionEvent routing over the viewer modal's action space (the SAME
// ids BmpViewerActivity registers its list/stepper hits with — production
// mapping, not a test-side duplicate).
TEST(ImageSettingsInput, TouchEventRoutesToOneSemanticAction) {
  using imageSettingsInput::kActionDecrement;
  using imageSettingsInput::kActionIncrement;
  using imageSettingsInput::kActionRow;
  using imageSettingsInput::settingsTouchActionFor;
  using imageSettingsInput::StepperTouch;
  EXPECT_EQ(settingsTouchActionFor(kActionRow), StepperTouch::ActivateRow);
  EXPECT_EQ(settingsTouchActionFor(kActionDecrement), StepperTouch::StepDown);
  EXPECT_EQ(settingsTouchActionFor(kActionIncrement), StepperTouch::StepUp);
  EXPECT_EQ(settingsTouchActionFor(0), StepperTouch::None);
  EXPECT_EQ(settingsTouchActionFor(99), StepperTouch::None);
  // The three ids are distinct, so one routed event can name only one action.
  EXPECT_NE(kActionRow, kActionDecrement);
  EXPECT_NE(kActionRow, kActionIncrement);
  EXPECT_NE(kActionDecrement, kActionIncrement);
}

// A stepper -/+ moves the focus onto the row it adjusts, then the shared
// draft-adjustment path derives the SAME parameter back from that row: the
// row<->param round trip must be exact for every ToneParam.
TEST(ImageSettingsInput, StepperParamRowRoundTrip) {
  using imageSettingsInput::editableParamForRow;
  using imageSettingsInput::editableRowForParam;
  const int paramCount = 4;
  for (int param = 0; param < paramCount; ++param) {
    const int row = editableRowForParam(param, paramCount);
    ASSERT_GE(row, 0) << "param=" << param;
    EXPECT_EQ(editableParamForRow(row, paramCount), param) << "param=" << param;
  }
  // Out-of-range values route nowhere.
  EXPECT_EQ(editableRowForParam(-1, paramCount), -1);
  EXPECT_EQ(editableRowForParam(paramCount, paramCount), -1);
}

// The modal panel is sized from its LARGEST page (Image Settings), computed
// from the same quantities the page renders with: title header + (paramCount
// + 3) rows — Reset + one per ToneParam + Apply + the sleep row. The old
// sizing guessed "6 data rows"; this pins the real layout arithmetic.
TEST(ImageSettingsInput, ModalBodyHeightCoversSettingsPage) {
  using imageSettingsInput::modalBodyHeight;
  // The current page layout: header + 7 rows at 36px.
  EXPECT_EQ(modalBodyHeight(30, 36, 4), 30 + 7 * 36);
  EXPECT_EQ(modalBodyHeight(0, 36, 4), 7 * 36);
  // Zero height/degenerate inputs stay additive (no hidden constants).
  EXPECT_EQ(modalBodyHeight(0, 0, 0), 3 * 0);
}

// DeleteConfirm page contract, over the PRODUCTION-owned routing
// (imageSettingsInput::deleteConfirmActionForRow — the exact function
// BmpViewerActivity::activateRow dispatches on): exactly two action rows,
// Cancel = action 0, Delete = action 1, anything else = nothing. The page
// builder (BmpViewerActivity::buildPageItems) lays the rows out as title
// header + Name label + up to two wrapped filename lines (all
// non-interactive, enabled=false), then those two action rows — so a row's
// modalRow/actionValue uniquely names the Cancel/Delete semantic action.
TEST(ImageSettingsInput, DeleteConfirmActionMapping) {
  using imageSettingsInput::DeleteAction;
  using imageSettingsInput::deleteConfirmActionForRow;
  EXPECT_EQ(deleteConfirmActionForRow(0), DeleteAction::Cancel);
  EXPECT_EQ(deleteConfirmActionForRow(1), DeleteAction::Delete);
  // Out-of-range rows (and the informational rows' absence of an action index)
  // activate nothing: the destructive action cannot fire from a fallback.
  EXPECT_EQ(deleteConfirmActionForRow(-1), DeleteAction::None);
  EXPECT_EQ(deleteConfirmActionForRow(2), DeleteAction::None);
  EXPECT_EQ(deleteConfirmActionForRow(3), DeleteAction::None);
}

// Fixed four-column stepper geometry (stepperColumns / stepperRequiredBodyWidth,
// pure policy): the touch Image Settings page shares ONE page-wide column set
// across every editable row, so the '-' control, the value slot and the '+'
// control hold identical X positions on all rows no matter what the current
// value text is.
TEST(ImageSettingsInput, StepperColumnsAreFixedByValueAndTouchMinimum) {
  using imageSettingsInput::stepperColumns;
  // Touch minimum wins when it exceeds the font-derived control width.
  const auto touch = stepperColumns(360, 8, 120, 90, 28, 44);
  EXPECT_EQ(touch.buttonWidth, 44);
  EXPECT_EQ(touch.valueWidth, 90 + 12);
  EXPECT_EQ(touch.gap, 6);
  EXPECT_TRUE(touch.fits);
  // Font-derived width wins when it exceeds the touch minimum.
  const auto large = stepperColumns(360, 8, 120, 90, 40, 44);
  EXPECT_EQ(large.buttonWidth, 50);
  // The controls do not depend on the label length: a longer label changes
  // only the fit decision, never the fixed control geometry.
  const auto longLabel = stepperColumns(360, 8, 200, 90, 28, 44);
  EXPECT_EQ(longLabel.buttonWidth, touch.buttonWidth);
  EXPECT_EQ(longLabel.valueWidth, touch.valueWidth);
  EXPECT_FALSE(longLabel.fits);
}

// Label/value measurements never move the controls: with the same value and
// touch inputs, different label widths produce identical button/value columns.
TEST(ImageSettingsInput, StepperColumnsIndependentOfLabelAndValueText) {
  using imageSettingsInput::stepperColumns;
  const auto small = stepperColumns(400, 8, 100, 80, 28, 44);
  const auto big = stepperColumns(400, 8, 180, 140, 28, 44);
  EXPECT_EQ(small.buttonWidth, big.buttonWidth);
  EXPECT_EQ(small.gap, big.gap);
  // Value slot scales with the widest VALUE text only (never the label):
  EXPECT_NE(small.valueWidth, big.valueWidth);
  // Same value on different bodies keeps identical columns.
  const auto narrow = stepperColumns(340, 8, 100, 80, 28, 44);
  EXPECT_EQ(small.buttonWidth, narrow.buttonWidth);
  EXPECT_EQ(small.valueWidth, narrow.valueWidth);
}

// Required body width is the exact inverse of the fit decision: the required
// width fits, one pixel less does not (deterministic fit boundary).
TEST(ImageSettingsInput, StepperRequiredBodyWidthMatchesFitBoundary) {
  using imageSettingsInput::stepperColumns;
  using imageSettingsInput::stepperRequiredBodyWidth;
  const int required = stepperRequiredBodyWidth(8, 120, 90, 28, 44);
  EXPECT_TRUE(stepperColumns(required, 8, 120, 90, 28, 44).fits);
  EXPECT_FALSE(stepperColumns(required - 1, 8, 120, 90, 28, 44).fits);
}
