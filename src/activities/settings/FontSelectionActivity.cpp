#include "FontSelectionActivity.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"

void FontSelectionActivity::onEnter() {
  Activity::onEnter();
  fontCount = fontFamilyOptionCount();
  selectedIndex =
      static_cast<int>(target == Target::TXT ? txtFontFamilyDynamicGetter(nullptr) : fontFamilyDynamicGetter(nullptr));
  if (selectedIndex >= fontCount) selectedIndex = 0;
  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNextList(selectedIndex, fontCount, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, fontCount, [this] { requestUpdate(); });
}

void FontSelectionActivity::handleSelection() {
  if (target == Target::TXT) {
    txtFontFamilyDynamicSetter(nullptr, static_cast<uint8_t>(selectedIndex));
  } else {
    fontFamilyDynamicSetter(nullptr, static_cast<uint8_t>(selectedIndex));
  }
  finish();
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  const StrId headerStr = target == Target::TXT ? StrId::STR_TXT_FONT_FAMILY : StrId::STR_FONT_FAMILY;
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(headerStr));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  const uint8_t activeIndex = static_cast<uint8_t>(target == Target::TXT ? txtFontFamilyDynamicGetter(nullptr)
                                                                         : fontFamilyDynamicGetter(nullptr));
  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, fontCount, selectedIndex,
      [](int index) { return fontFamilyOptionLabel(static_cast<uint8_t>(index)); }, nullptr, nullptr,
      [activeIndex](int index) -> std::string { return index == activeIndex ? tr(STR_SELECTED) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
