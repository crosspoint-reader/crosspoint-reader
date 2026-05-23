#include "QuickOverridesActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

QuickOverridesActivity::QuickOverridesActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const int8_t initialEmbeddedStyleOverride,
    const int8_t initialImageRenderingOverride, const int8_t initialFontFamilyOverride,
    const std::string& initialSdFontFamilyOverride, const int8_t initialFontSizeOverride,
    const int8_t initialBionicReadingOverride, const int8_t initialParagraphAlignmentOverride,
    const int8_t initialTextAntiAliasingOverride, const int8_t initialHyphenationOverride)
    : MenuListActivity("QuickOverrides", renderer, mappedInput),
      pendingEmbeddedStyleOverride(initialEmbeddedStyleOverride),
      pendingImageRenderingOverride(initialImageRenderingOverride),
      pendingFontFamilyOverride(initialFontFamilyOverride),
      pendingSdFontFamilyOverride(initialSdFontFamilyOverride),
      pendingFontSizeOverride(initialFontSizeOverride),
      pendingBionicReadingOverride(initialBionicReadingOverride),
      pendingParagraphAlignmentOverride(initialParagraphAlignmentOverride),
      pendingTextAntiAliasingOverride(initialTextAntiAliasingOverride),
      pendingHyphenationOverride(initialHyphenationOverride) {
  buildMenuItems();
}

namespace {

// Three-state cycle helper for overrides represented as int8_t with -1 = default.
//   slot 0 -> -1 (default)
//   slot 1 -> 1  (on)
//   slot 2 -> 0  (off)
uint8_t threeStateSlotFromOverride(int8_t value) {
  if (value < 0) return 0;
  if (value > 0) return 1;
  return 2;
}

int8_t threeStateOverrideFromSlot(uint8_t slot) {
  if (slot == 0) return -1;
  if (slot == 1) return 1;
  return 0;
}

}  // namespace

void QuickOverridesActivity::buildMenuItems() {
  menuItems.reserve(8);

  auto* self = this;

  // Embedded style: default / on / off
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_EMBEDDED_STYLE, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const QuickOverridesActivity*>(ctx)->pendingEmbeddedStyleOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<QuickOverridesActivity*>(ctx)->pendingEmbeddedStyleOverride = threeStateOverrideFromSlot(v);
      }));

  // Image rendering: default(-1) / display(0) / placeholder(1) / suppress(2)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_IMAGES,
      {StrId::STR_DEFAULT_VALUE, StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
      self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const QuickOverridesActivity*>(ctx);
        return (s->pendingImageRenderingOverride < 0) ? 0 : (s->pendingImageRenderingOverride + 1);
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<QuickOverridesActivity*>(ctx);
        s->pendingImageRenderingOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
      }));

  // Font family: built-in only — preserves any SD-family override unless the user
  // explicitly picks a built-in (which clears it, matching the storage semantics).
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_FONT_FAMILY, {StrId::STR_DEFAULT_VALUE, StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS}, self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const QuickOverridesActivity*>(ctx);
        return (s->pendingFontFamilyOverride < 0) ? 0 : static_cast<uint8_t>(s->pendingFontFamilyOverride + 1);
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<QuickOverridesActivity*>(ctx);
        if (v == 0) {
          s->pendingFontFamilyOverride = -1;
        } else {
          s->pendingFontFamilyOverride = static_cast<int8_t>(v - 1);
          s->pendingSdFontFamilyOverride.clear();
        }
      }));

  // Font size: default(-1) -> Small(0) -> Medium(1) -> Large(2) -> X Large(3) -> Tiny(4)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_FONT_SIZE,
      {StrId::STR_DEFAULT_VALUE, StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE,
       StrId::STR_TINY},
      self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const QuickOverridesActivity*>(ctx);
        return (s->pendingFontSizeOverride < 0) ? 0 : static_cast<uint8_t>(s->pendingFontSizeOverride + 1);
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<QuickOverridesActivity*>(ctx);
        s->pendingFontSizeOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
      }));

  // Bionic: default / on / off
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_BIONIC_READING, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const QuickOverridesActivity*>(ctx)->pendingBionicReadingOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<QuickOverridesActivity*>(ctx)->pendingBionicReadingOverride = threeStateOverrideFromSlot(v);
      }));

  // Paragraph alignment: default(-1) + the 5 global options
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_PARA_ALIGNMENT,
      {StrId::STR_DEFAULT_VALUE, StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
       StrId::STR_BOOK_S_STYLE},
      self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const QuickOverridesActivity*>(ctx);
        return (s->pendingParagraphAlignmentOverride < 0)
                   ? 0
                   : static_cast<uint8_t>(s->pendingParagraphAlignmentOverride + 1);
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<QuickOverridesActivity*>(ctx);
        s->pendingParagraphAlignmentOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
      }));

  // Text anti-aliasing: default / on / off
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_TEXT_AA, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const QuickOverridesActivity*>(ctx)->pendingTextAntiAliasingOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<QuickOverridesActivity*>(ctx)->pendingTextAntiAliasingOverride = threeStateOverrideFromSlot(v);
      }));

  // Hyphenation: default / on / off
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_HYPHENATION, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(static_cast<const QuickOverridesActivity*>(ctx)->pendingHyphenationOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<QuickOverridesActivity*>(ctx)->pendingHyphenationOverride = threeStateOverrideFromSlot(v);
      }));
}

void QuickOverridesActivity::onEnter() {
  MenuListActivity::onEnter();
  // Fast refresh keeps the menu feeling responsive when cycling options. The
  // parent reader is responsible for its own full refresh on resume if any
  // override actually changed (applyBookReaderOverrides short-circuits if not).
  renderer.setNextDisplayRefreshMode(HalDisplay::FAST_REFRESH);
}

void QuickOverridesActivity::onSettingToggled(int /*index*/) {
  // No persistence on each toggle — overrides are committed on exit through
  // the result handler in the parent reader activity.
  renderer.setNextDisplayRefreshMode(HalDisplay::FAST_REFRESH);
}

void QuickOverridesActivity::onBackPressed() { finishWithResult(/*cancelled=*/false); }

void QuickOverridesActivity::finishWithResult(bool cancelled) {
  ActivityResult result;
  result.isCancelled = cancelled;
  MenuResult payload;
  payload.action = -1;
  payload.nameId = -1;
  payload.embeddedStyleOverride = pendingEmbeddedStyleOverride;
  payload.imageRenderingOverride = pendingImageRenderingOverride;
  payload.fontFamilyOverride = pendingFontFamilyOverride;
  payload.sdFontFamilyOverride = pendingSdFontFamilyOverride;
  payload.fontSizeOverride = pendingFontSizeOverride;
  payload.bionicReadingOverride = (pendingBionicReadingOverride > 0) ? 1 : 0;
  payload.paragraphAlignmentOverride = pendingParagraphAlignmentOverride;
  payload.textAntiAliasingOverride = pendingTextAntiAliasingOverride;
  payload.hyphenationOverride = pendingHyphenationOverride;
  result.data = std::move(payload);
  setResult(std::move(result));
  finish();
}

void QuickOverridesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  const std::string title = tr(STR_QUICK_OVERRIDES);
  const int titleX = contentRect.x +
                     (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, title.c_str(), true, EpdFontFamily::BOLD);

  const int startY = 50 + contentRect.y;
  const int listHeight = contentRect.height - (startY - contentRect.y);
  drawMenuList(Rect{contentRect.x, startY, contentRect.width, listHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
