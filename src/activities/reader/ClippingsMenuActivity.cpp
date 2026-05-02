#include "ClippingsMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 3;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CLIP_NAV_MODE, StrId::STR_ANNOT_SHOW, StrId::STR_CLIPPING_STORAGE};
}  // namespace

void ClippingsMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ClippingsMenuActivity::onExit() { Activity::onExit(); }

void ClippingsMenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleSetting(selectedIndex);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });
}

void ClippingsMenuActivity::toggleSetting(int idx) {
  if (idx == 0) {
    SETTINGS.clipNavMode = (SETTINGS.clipNavMode + 1) % CrossPointSettings::CLIP_NAV_MODE_COUNT;
  } else if (idx == 1) {
    SETTINGS.annotationVisibility = (SETTINGS.annotationVisibility + 1) % 2;
  } else if (idx == 2) {
    SETTINGS.clippingStorage = (SETTINGS.clippingStorage + 1) % CrossPointSettings::CLIPPING_STORAGE_COUNT;
  }
  SETTINGS.saveToFile();
}

void ClippingsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CAT_CLIPPINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [this](int index) {
        if (index == 0) {
          return std::string(SETTINGS.clipNavMode == CrossPointSettings::LINE_AWARE ? tr(STR_CLIP_NAV_LINE)
                                                                                    : tr(STR_CLIP_NAV_WORD));
        } else if (index == 1) {
          return std::string(SETTINGS.annotationVisibility ? tr(STR_STATE_OFF) : tr(STR_STATE_ON));
        } else if (index == 2) {
          return std::string(SETTINGS.clippingStorage == CrossPointSettings::SINGLE_FILE ? tr(STR_CLIPPING_SINGLE_FILE)
                                                                                         : tr(STR_CLIPPING_PER_BOOK));
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
