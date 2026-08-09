#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"

// Shared glue for activities hosting a FreeInkApp: the font-bound render
// target and the touch snapshot FreeInkApp routing consumes.

// One app-wide ThemeTokens instance shared by every FreeInkApp via
// setThemeRef: the tokens are identical on every screen, so per-app copies
// (~1.5KB each, and one per stacked activity) were pure heap waste. Refreshed
// on every screen entry, so theme or font changes between activities
// re-derive it; live theme changes (Settings) refresh it in place and every
// referencing app repaints in the new look.
inline freeink::ui::ThemeTokens& sharedUiThemeTokens() {
  static freeink::ui::ThemeTokens tokens;
  return tokens;
}

// Refresh the shared tokens from the active UITheme + this target's fonts and
// point the app at them. Replaces the old per-app `app.setTheme(...)` copies.
template <typename App>
inline void applySharedUiTheme(App& app, const freeink::ui::GfxRendererTarget& target) {
  sharedUiThemeTokens() = uiThemeTokens(target);
  app.setThemeRef(&sharedUiThemeTokens());
}

// Bind the uiScale fonts before FreeInkApp's constructor derives its theme
// metrics from the body font's line height.
inline freeink::ui::GfxRendererTarget makeUiTarget(const GfxRenderer& renderer) {
  freeink::ui::GfxRendererTarget target(renderer);
  const auto spec = uiScaleSpec();
  target.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, spec.titleFontId);
  return target;
}

// Tap release with coords, plus the raw release the tap classifier never
// reports (swipe end, drag-off) delivered off-target: nothing dispatches,
// but routing drops its pressed-element state instead of ghosting it onto
// the next render.
// Firmware UIIcon -> FreeInkUI bitmap for list rows (SDK-format icons only;
// the legacy drawIcon assets use a different bit layout). Two crisp sizes:
// 24 for single-line rows, 32 for label+subtitle rows.
inline freeink::ui::BitmapRef listIconFor(const UIIcon icon, const int size = 24) {
  if (size >= 32) {
    switch (icon) {
      case UIIcon::Folder:
        return freeink::ui::bitmapFromIcon(icon_folder_32);
      case UIIcon::Text:
        return freeink::ui::bitmapFromIcon(icon_file_text_32);
      case UIIcon::Image:
        return freeink::ui::bitmapFromIcon(icon_image_32);
      case UIIcon::Book:
        return freeink::ui::bitmapFromIcon(icon_book_32);
      case UIIcon::File:
        return freeink::ui::bitmapFromIcon(icon_file_32);
      case UIIcon::Wifi:
        return freeink::ui::bitmapFromIcon(icon_wifi_32);
      case UIIcon::Library:
        return freeink::ui::bitmapFromIcon(icon_library_32);
      case UIIcon::Hotspot:
        return freeink::ui::bitmapFromIcon(icon_radio_tower_32);
      case UIIcon::Bookmark:
        return freeink::ui::bitmapFromIcon(icon_bookmark_32);
      default:
        return {};
    }
  }
  switch (icon) {
    case UIIcon::Folder:
      return freeink::ui::bitmapFromIcon(icon_folder_24);
    case UIIcon::Text:
      return freeink::ui::bitmapFromIcon(icon_file_text_24);
    case UIIcon::Image:
      return freeink::ui::bitmapFromIcon(icon_image_24);
    case UIIcon::Book:
      return freeink::ui::bitmapFromIcon(icon_book_24);
    case UIIcon::File:
      return freeink::ui::bitmapFromIcon(icon_file_24);
    case UIIcon::Wifi:
      return freeink::ui::bitmapFromIcon(icon_wifi_24);
    case UIIcon::Library:
      return freeink::ui::bitmapFromIcon(icon_library_24);
    case UIIcon::Hotspot:
      return freeink::ui::bitmapFromIcon(icon_radio_tower_24);
    case UIIcon::Bookmark:
      return freeink::ui::bitmapFromIcon(icon_bookmark_24);
    default:
      return {};
  }
}

// Bottom-anchored Cancel / OK pair for slider dialogs on touch devices, where
// the physical Back/Confirm buttons (and their auto-hidden hints) may not
// exist. Callers gate on hasTouch(): button boards keep the hint chrome and
// need no on-screen pair. Consumes the bottom of the screen's content band.
template <typename Screen>
inline void addDialogCancelOk(Screen& screen, const freeink::ui::ActionId cancelAction,
                              const freeink::ui::ActionId okAction) {
  const auto& theme = screen.theme();
  const int16_t sideInset = static_cast<int16_t>(theme.spaceLg * 2);
  const freeink::ui::Rect band =
      screen.takeBottom(theme.rowHeight, theme.spaceLg).inset(freeink::ui::Insets{0, sideInset, 0, sideInset});
  const int16_t gap = theme.spaceLg;
  const int16_t buttonWidth = static_cast<int16_t>((band.width - gap) / 2);

  freeink::ui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = cancelAction;
  cancel.inputMask = freeink::ui::InputTouch;
  cancel.text = theme.bodyText;
  freeink::ui::ButtonProps ok = cancel;
  ok.label = tr(STR_OK_BUTTON);
  ok.action = okAction;
  freeink::ui::button(screen.frame(), freeink::ui::Rect{band.x, band.y, buttonWidth, band.height}, cancel);
  freeink::ui::button(
      screen.frame(),
      freeink::ui::Rect{static_cast<int16_t>(band.x + band.width - buttonWidth), band.y, buttonWidth, band.height}, ok);
}

// Scroll semantics shared by every FreeInkUI list screen: swipes move the
// viewport (topIndex) without touching the selection; button navigation moves
// the selection and pulls the viewport along just enough to keep it visible.

inline int scrollListBy(const int topIndex, const int delta, const int visibleRows, const int count) {
  int maxTop = count - visibleRows;
  if (maxTop < 0) maxTop = 0;
  int next = topIndex + delta;
  if (next > maxTop) next = maxTop;
  if (next < 0) next = 0;
  return next;
}

inline int followListSelection(const int selectedIndex, const int topIndex, const int visibleRows, const int count) {
  return static_cast<int>(freeink::ui::listTopIndexFor(
      static_cast<int16_t>(selectedIndex), static_cast<uint16_t>(topIndex),
      static_cast<uint16_t>(visibleRows > 0 ? visibleRows : 1), static_cast<uint16_t>(count)));
}

// withLongPress: the SDK touch classifier fires the long-press WHILE the
// finger is still down (matching the physical-button hold-to-act feel) and
// suppresses the remainder of the contact, so the finger lift can't also
// tap-dismiss the popup the long-press opens. Delivered as a touchReleased +
// longPress snapshot at the contact point; only rows masked InputLongPress
// receive it. Mirrors the SDK's long-press-aware fui::snapshotFrom, but maps
// coordinates through the renderer's LIVE orientation (the reader rotates at
// runtime), which the DeviceContext-based SDK adapter does not track.
inline freeink::ui::InputSnapshot touchSnapshotFrom(const MappedInputManager& mappedInput,
                                                    const bool withLongPress = false) {
  int tx = 0;
  int ty = 0;
  if (withLongPress && mappedInput.wasScreenLongPress(tx, ty)) {
    freeink::ui::InputSnapshot snap{};
    snap.touchReleased = true;
    snap.longPress = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
    return snap;
  }

  freeink::ui::InputSnapshot snap{};
  // Live contact position: only InputDrag-masked elements (sliders) react, so
  // carrying it in every snapshot is free for ordinary screens.
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    snap.touchHeld = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    snap.touchPressed = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    snap.touchReleased = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  } else if (mappedInput.wasScreenTouchReleased()) {
    snap.touchReleased = true;
    snap.touchX = -1;
    snap.touchY = -1;
  }
  return snap;
}
