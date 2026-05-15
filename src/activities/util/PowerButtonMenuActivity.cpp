#include "PowerButtonMenuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PowerButtonMenuActivity::onEnter() {
  Activity::onEnter();
  s_isActive = true;
  selectedIndex = 0;
  requestUpdate();
}

void PowerButtonMenuActivity::onExit() {
  Activity::onExit();
  s_isActive = false;
}

void PowerButtonMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    APP_STATE.pendingPwrBtnAction = static_cast<uint8_t>(items[selectedIndex].action);
    LOG_DBG("PWRMENU", "Selected action: %d", APP_STATE.pendingPwrBtnAction);
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void PowerButtonMenuActivity::render(RenderLock&&) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const int itemCount = static_cast<int>(items.size());
  constexpr int dialogPadding = 20;
  constexpr int titleHeight = 40;
  constexpr int itemHeight = 35;
  constexpr int hintsHeight = 40;
  constexpr int dialogMarginTop = 15;
  constexpr int dialogMarginBottom = 10;

  const int contentHeight = titleHeight + (itemCount * itemHeight) + hintsHeight;
  const int dialogHeight = dialogMarginTop + contentHeight + dialogMarginBottom;

  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_ACTION_MENU), EpdFontFamily::BOLD);
  for (const auto& item : items) {
    const int w = renderer.getTextWidth(UI_10_FONT_ID, I18N.get(item.labelId));
    if (w > maxTextWidth) {
      maxTextWidth = w;
    }
  }
  const int dialogWidth = maxTextWidth + dialogPadding * 2 + 20;

  const int dialogX = (screenWidth - dialogWidth) / 2;
  const int dialogY = (screenHeight - dialogHeight) / 2;

  GUI.drawDialogBackground(renderer, Rect{dialogX, dialogY, dialogWidth, dialogHeight});

  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_ACTION_MENU), EpdFontFamily::BOLD);
  const int titleX = dialogX + (dialogWidth - titleWidth) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, dialogY + dialogMarginTop + 5, tr(STR_ACTION_MENU), true,
                    EpdFontFamily::BOLD);

  const int separator1Y = dialogY + dialogMarginTop + titleHeight;
  renderer.drawLine(dialogX + dialogPadding, separator1Y, dialogX + dialogWidth - dialogPadding, separator1Y);

  const int itemStartY = separator1Y + 5;
  for (int i = 0; i < itemCount; i++) {
    const int itemY = itemStartY + (i * itemHeight);
    if (i == selectedIndex) {
      GUI.drawPopupSelection(renderer,
                            Rect{dialogX + dialogPadding, itemY, dialogWidth - dialogPadding * 2, itemHeight - 2},
                            I18N.get(items[i].labelId));
    } else {
      renderer.drawText(UI_10_FONT_ID, dialogX + dialogPadding + 12, itemY + 8, I18N.get(items[i].labelId));
    }
  }

  const int separator2Y = itemStartY + (itemCount * itemHeight);
  renderer.drawLine(dialogX + dialogPadding, separator2Y, dialogX + dialogWidth - dialogPadding, separator2Y);

  const int hintsY = separator2Y + 8;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const int hintAreaWidth = dialogWidth - dialogPadding * 2;
  const int singleHintWidth = hintAreaWidth / 4;
  const char* hintLabels[] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};

  for (int i = 0; i < 4; i++) {
    if (hintLabels[i] != nullptr && hintLabels[i][0] != '\0') {
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, hintLabels[i]);
      const int hintCenterX = dialogX + dialogPadding + (i * singleHintWidth) + singleHintWidth / 2;
      renderer.drawText(UI_10_FONT_ID, hintCenterX - textWidth / 2, hintsY, hintLabels[i]);
    }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
