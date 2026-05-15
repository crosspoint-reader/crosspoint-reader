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
    APP_STATE.pendingPwrBtnAction = 10 + static_cast<uint8_t>(items[selectedIndex].action);
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
  constexpr int innerPadding = 16;
  constexpr int itemSpacing = 6;
  constexpr int selectionHPadding = 8;
  constexpr int selectionVPadding = 4;

  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int itemHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int listHeight = itemHeight * itemCount + itemSpacing * (itemCount - 1);
  const int contentHeight = titleHeight + 10 + listHeight;
  const int dialogHeight = contentHeight + innerPadding * 2;

  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_ACTION_MENU), EpdFontFamily::BOLD);
  for (const auto& item : items) {
    const int w = renderer.getTextWidth(UI_10_FONT_ID, I18N.get(item.labelId), EpdFontFamily::BOLD);
    if (w > maxTextWidth) {
      maxTextWidth = w;
    }
  }
  const int dialogWidth = std::min((maxTextWidth + innerPadding * 2) * 12 / 10, screenWidth - 20);

  const int dialogX = (screenWidth - dialogWidth) / 2;
  const int dialogY = (screenHeight - dialogHeight) / 2;

  GUI.drawDialogBackground(renderer, Rect{dialogX, dialogY, dialogWidth, dialogHeight});

  renderer.drawCenteredText(UI_12_FONT_ID, dialogY + innerPadding, tr(STR_ACTION_MENU), true, EpdFontFamily::BOLD);

  int y = dialogY + innerPadding + titleHeight + 10;
  for (int i = 0; i < itemCount; i++) {
    const int itemY = y + i * (itemHeight + itemSpacing);
    const bool selected = (i == selectedIndex);
    const char* labelText = I18N.get(items[i].labelId);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, labelText, EpdFontFamily::BOLD);
    const int labelX = dialogX + (dialogWidth - labelWidth) / 2;

    Rect itemRect(labelX - selectionHPadding, itemY - selectionVPadding, labelWidth + selectionHPadding * 2,
                  itemHeight + selectionVPadding * 2);
    GUI.drawPopupSelection(renderer, UI_10_FONT_ID, itemRect, labelText, selected);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
