#include "ClippingsMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClippingsMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ClippingsMenuActivity::onExit() { Activity::onExit(); }

// Item 0: Navigation       (clipNavMode)
// Item 1: Show Annotations (annotationVisibility)
// Item 2: Storage          (clippingStorage)

void ClippingsMenuActivity::toggleSetting(int idx) {
  switch (idx) {
    case 0:
      SETTINGS.clipNavMode = (SETTINGS.clipNavMode + 1) % CrossPointSettings::CLIP_NAV_MODE_COUNT;
      break;
    case 1:
      SETTINGS.annotationVisibility = (SETTINGS.annotationVisibility + 1) % 2;
      break;
    case 2:
      SETTINGS.clippingStorage = (SETTINGS.clippingStorage + 1) % CrossPointSettings::CLIPPING_STORAGE_COUNT;
      break;
    default:
      break;
  }
  SETTINGS.saveToFile();
}

const char* ClippingsMenuActivity::settingValue(int idx) const {
  switch (idx) {
    case 0:
      return SETTINGS.clipNavMode == CrossPointSettings::LINE_AWARE ? tr(STR_CLIP_NAV_LINE) : tr(STR_CLIP_NAV_WORD);
    case 1:
      return SETTINGS.annotationVisibility ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case 2:
      return SETTINGS.clippingStorage == CrossPointSettings::SINGLE_FILE ? tr(STR_CLIPPING_SINGLE_FILE)
                                                                         : tr(STR_CLIPPING_PER_BOOK);
    default:
      return "";
  }
}

void ClippingsMenuActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleSetting(selectedIndex);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ClippingsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_CAT_CLIPPINGS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_CAT_CLIPPINGS), true, EpdFontFamily::BOLD);

  static const StrId labels[ITEM_COUNT] = {StrId::STR_CLIP_NAV_MODE, StrId::STR_ANNOT_SHOW,
                                           StrId::STR_CLIPPING_STORAGE};

  const int startY = 60 + contentY;
  constexpr int lineHeight = 32;

  for (int i = 0; i < ITEM_COUNT; ++i) {
    const int displayY = startY + i * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY + 6, I18N.get(labels[i]), !isSelected);

    const char* val = settingValue(i);
    const int valW = renderer.getTextWidth(UI_10_FONT_ID, val);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - valW, displayY + 6, val, !isSelected);
  }

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  renderer.displayBuffer();
}
