#pragma once

#include <cstdint>

// Image Settings page input policy (image viewer).
//
// The Image Settings page is 2-axis: the vertical axis chooses the setting row
// (the fixed side Up/Down keys), the horizontal axis modifies the selected
// value (the user-configured front Left/Right keys). MappedInputManager's
// merged list navigation (NavNext = Down || Right, NavPrevious = Up || Left)
// is intentionally NOT used on this page: it would make one front Left/Right
// event match both the value branch and the row-selection branch. Here one
// button event maps to EXACTLY ONE semantic action; no event swallowing,
// debounce, or latches are involved.
namespace imageSettingsInput {

enum class Button : uint8_t { None = 0, Up, Down, Left, Right, Confirm, Back };
enum class Action : uint8_t { None = 0, RowUp, RowDown, ValueDown, ValueUp, Activate, Back };

// Map one button event (press edge first, then release edge) to its single
// semantic action. Press edges carry the four directional axes; release edges
// carry the two action buttons. Confirm/Back tolerate a both-edges dispatch
// window: a capacitive Home-key tap (Confirm) or a back swipe is ONE event
// that MappedInputManager reports on both wasPressed and wasReleased, and it
// must still fire — while a press-only edge never acts (buttons act on
// release).
inline Action actionFor(const Button button, const bool press, const bool release) {
  if (press) {
    switch (button) {
      case Button::Up:
        return Action::RowUp;
      case Button::Down:
        return Action::RowDown;
      case Button::Left:
        return Action::ValueDown;
      case Button::Right:
        return Action::ValueUp;
      default:
        break;  // Confirm/Back fall through to the release edge below
    }
  }
  if (release) {
    switch (button) {
      case Button::Confirm:
        return Action::Activate;
      case Button::Back:
        return Action::Back;
      default:
        return Action::None;
    }
  }
  return Action::None;
}

// Confirm-activation routing for the Image Settings page rows (pure policy).
// Row order (paramCount = editable B/G/C/Q rows, currently 4):
//   0                = Reset to defaults
//   1..paramCount    = editable value rows (Brightness/Gamma/Contrast/Quantizer)
//   paramCount + 1   = explicit Apply row
//   paramCount + 2   = Use for sleep rendering
// Every editable value row AND the explicit Apply row share the one
// viewer-Apply route; the dedicated sleep row is the only save-profile route;
// Reset only restages the draft; anything out of range activates nothing.
// Mutually exclusive by construction. The explicit mapping (NOT
// static_cast<ToneParam>(row) arithmetic) is what keeps the value rows aligned
// after the Reset row shifted every index by one.
enum class RowAction : uint8_t { None = 0, ResetDraft, ApplyViewer, SaveSleepProfile };
inline RowAction confirmActionForRow(const int row, const int paramCount) {
  if (row < 0 || row > paramCount + 2) return RowAction::None;
  if (row == 0) return RowAction::ResetDraft;
  if (row == paramCount + 2) return RowAction::SaveSleepProfile;
  return RowAction::ApplyViewer;  // editable rows AND the explicit Apply row
}

// Editable parameter index for a settings row (0..paramCount-1 maps to
// ToneParam), or -1 for the action rows (Reset / Apply / Use for sleep
// rendering) which Left/Right must never step.
inline int editableParamForRow(const int row, const int paramCount) {
  if (row < 1 || row > paramCount) return -1;
  return row - 1;
}

// Image Settings UI step policy: one Left/Right press moves a percent-encoded
// value by TONE_STEP, clamped to the parameter range. Lives in the UI layer —
// the persisted profile/ToneLut layer (ToneLut.h) knows only the min/max
// constraints, never the step the UI walks with.
inline constexpr uint8_t TONE_STEP = 5;
inline uint8_t stepClamped(const uint8_t value, const int delta, const uint8_t lo, const uint8_t hi) {
  const int stepped = value + TONE_STEP * delta;
  return static_cast<uint8_t>(stepped < lo ? lo : (stepped > hi ? hi : stepped));
}

// Image Settings page availability (pure policy): the B/G/C/Q pipeline is
// consumed ONLY by the BMP decode path (Bitmap + ToneLut). PNG decodes through
// PngToFramebufferConverter, which has no tone hook, so offering the page for
// PNG would let a user persist settings that never affect the image.
inline bool imageSettingsAvailable(const bool toneCapableFormat) { return toneCapableFormat; }

// Quantizer row presentation (pure policy over the persisted domain):
//   -1 = no override / controller-specific fallback -> Default
//    0 = explicit legacy threshold set               -> Legacy
//    1 = explicit canonical threshold set            -> Canonical
// The display is derived from the persisted value alone, so it can never
// disagree with it (a -1 override must NOT read as "Legacy": on a controller
// whose fallback is canonical that would be wrong).
enum class QuantizerLabel : uint8_t { Default = 0, Legacy, Canonical };
inline QuantizerLabel quantizerLabel(const int8_t quantizer) {
  if (quantizer == 0) return QuantizerLabel::Legacy;
  if (quantizer == 1) return QuantizerLabel::Canonical;
  return QuantizerLabel::Default;
}

// Directional quantizer step (pure policy): ONE cyclic space walked in both
// directions — the two visually different stepper controls must move opposite
// ways, and the hardware Right key must still land on Canonical first from
// the Default (-1) state, as it always did:
//   +1: Default -> Canonical -> Legacy -> Default
//   -1: Default -> Legacy -> Canonical -> Default
// Persisted domain unchanged: -1 = Default, 0 = Legacy, 1 = Canonical.
// Unknown values (not UI-reachable; the load path normalizes them) act as
// Default.
inline int8_t quantizerStepped(const int8_t current, const int delta) {
  constexpr int8_t cycle[3] = {-1, 1, 0};  // Default, Canonical, Legacy
  int idx = 0;
  for (int i = 0; i < 3; ++i) {
    if (cycle[i] == current) idx = i;
  }
  const int step = delta >= 0 ? 1 : 2;
  return cycle[(idx + step) % 3];
}

// Modal panel body height (pure policy, host-tested): the panel must fit its
// LARGEST page — Image Settings — laid out from the same quantities it
// renders with: the title header plus the page's selectable rows (Reset + one
// per ToneParam + Apply + the sleep row = paramCount + 3) at the row cadence.
// No row-count guess lives in the sizing path.
inline int modalBodyHeight(const int headerHeight, const int rowHeight, const int paramCount) {
  return headerHeight + (paramCount + 3) * rowHeight;
}

// Modal row cadence (pure policy, host-tested): on touch-capable targets the
// visual row height must be at least the device's touch minimum, so
// ensureMinTouchRect() never expands one row's hit band into the neighbor's
// — newest-first routing would otherwise hand boundary taps to the LATER row
// (e.g. a tap at the bottom edge of a DeleteConfirm Cancel resolving to
// Delete). Button-only targets have no touch minimum to satisfy and keep the
// 36px list density. Both inputs are DeviceContext primitives.
constexpr int MODAL_MIN_ROW_H = 36;
inline constexpr int modalRowHeight(const bool touchCapable, const int minTouchSize) {
  return touchCapable && minTouchSize > MODAL_MIN_ROW_H ? minTouchSize : MODAL_MIN_ROW_H;
}

// Editable row index for a ToneParam (the inverse of editableParamForRow): a
// touch stepper +/- moves the visible focus onto the row it adjusts, so the
// adjustment and the row highlight stay consistent. -1 for out-of-range.
inline int editableRowForParam(const int param, const int paramCount) {
  if (param < 0 || param >= paramCount) return -1;
  return param + 1;  // row 0 is Reset to defaults
}

// Touch ActionEvent routing for the viewer modal (pure policy; the activity
// uses these ids for every list hit it registers). One routed event maps to
// exactly ONE semantic action:
//   kActionRow      value = selectable row index — the row Confirm activates
//                   (Confirm parity: modalRow = value, then activateRow()).
//   kActionDecrement/Increment
//                   value = ToneParam index — the row that parameter's
//                   stepper +/- adjusts (focus moves onto that row, then the
//                   shared draft-adjustment path runs).
enum ModalActionId : uint16_t {
  kActionRow = 1,
  kActionDecrement = 2,
  kActionIncrement = 3,
};

enum class StepperTouch : uint8_t { None = 0, ActivateRow, StepDown, StepUp };
inline StepperTouch settingsTouchActionFor(const uint16_t action) {
  if (action == kActionRow) return StepperTouch::ActivateRow;
  if (action == kActionDecrement) return StepperTouch::StepDown;
  if (action == kActionIncrement) return StepperTouch::StepUp;
  return StepperTouch::None;
}

// DeleteConfirm page routing (pure policy, production-owned): the page shows
// informational rows first (title header + Name label + up to two wrapped
// filename lines — built non-interactive by BmpViewerActivity), then exactly
// two action rows: Cancel = action 0 (back to Options), Delete = action 1.
// Anything else activates nothing, so the destructive action is reachable only
// through the explicit Delete action — never through a visual-row fallback.
enum class DeleteAction : uint8_t { None = 0, Cancel, Delete };
inline DeleteAction deleteConfirmActionForRow(const int row) {
  switch (row) {
    case 0:
      return DeleteAction::Cancel;
    case 1:
      return DeleteAction::Delete;
    default:
      return DeleteAction::None;
  }
}

}  // namespace imageSettingsInput
