#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>

#include <atomic>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"

// Shared glue for activities hosting a FreeInkApp: the font-bound render
// target and the touch snapshot FreeInkApp routing consumes.

// One app-wide ThemeTokens instance shared by every FreeInkApp via
// setThemeRef, so per-app copies (~1.5KB each, and one per stacked activity)
// aren't pure heap waste. Refreshed on every screen entry, so theme or font
// changes between activities re-derive it; live theme changes (Settings)
// refresh it and every referencing app repaints in the new look.
//
// Backed by a small pool + an atomic cell (FreeInkApp::setThemeRef() takes a
// pointer to the cell, not to a ThemeTokens instance directly) rather than a
// single instance overwritten in place: refreshSharedUiThemeTokens() below
// always builds the new tokens into whichever pool slot the cell does NOT
// currently reference, then does one atomic store. Every app sharing the
// cell picks up the change on its next theme() call, and nothing ever
// dereferences an instance mid-overwrite — unlike a plain
// `sharedTokens = uiThemeTokens(target);` in-place assignment, which a
// render task reading theme().rowHeight/etc. field-by-field on another task
// could observe as a torn mix of old and new fields.
inline std::atomic<const freeink::ui::ThemeTokens*>& sharedUiThemeCell() {
  static std::atomic<const freeink::ui::ThemeTokens*> cell{nullptr};
  return cell;
}

// Rebuilds the shared tokens for `target` and atomically publishes them via
// sharedUiThemeCell(). Returns the freshly-published instance for callers
// that also want to read it back immediately (e.g. BaseTheme::drawHeader(),
// which derives the same tokens as a render-path scratch value instead of
// stack-allocating its own copy).
inline const freeink::ui::ThemeTokens& refreshSharedUiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  static freeink::ui::ThemeTokens pool[2];
  auto& cell = sharedUiThemeCell();
  const auto* current = cell.load(std::memory_order_relaxed);
  freeink::ui::ThemeTokens* next = (current == &pool[0]) ? &pool[1] : &pool[0];
  *next = uiThemeTokens(target);
  cell.store(next, std::memory_order_release);
  return *next;
}

// Refresh the shared tokens from the active UITheme + this target's fonts and
// point the app at them. Replaces the old per-app `app.setTheme(...)` copies.
template <typename App>
inline void applySharedUiTheme(App& app, const freeink::ui::GfxRendererTarget& target) {
  refreshSharedUiThemeTokens(target);
  app.setThemeRef(&sharedUiThemeCell());
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
      case UIIcon::Usb:
        return freeink::ui::bitmapFromIcon(icon_usb_32);
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
    case UIIcon::Usb:
      return freeink::ui::bitmapFromIcon(icon_usb_24);
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
