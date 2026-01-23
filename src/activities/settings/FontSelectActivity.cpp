#include "FontSelectActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void FontSelectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Wait for parent activity's rendering to complete (screen refresh takes ~422ms)
  // Wait 500ms to be safe and avoid race conditions with parent activity
  vTaskDelay(500 / portTICK_PERIOD_MS);

  // Scan fonts
  FontMgr.scanFonts();

  // Total items = 1 (Built-in) + external font count
  totalItems = 1 + FontMgr.getFontCount();

  // Set current selection based on mode
  int currentFont = (mode == SelectMode::Reader) ? FontMgr.getSelectedIndex()
                                                 : FontMgr.getUiSelectedIndex();
  if (currentFont < 0) {
    selectedIndex = 0; // Built-in
  } else {
    selectedIndex = currentFont + 1; // External font index + 1
  }

  // 同步渲染，不使用后台任务
  render();
}

void FontSelectActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // 不需要清理任务和 mutex，因为我们不再使用它们
}

void FontSelectActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  bool needsRender = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + totalItems - 1) % totalItems;
    needsRender = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % totalItems;
    needsRender = true;
  }

  // 同步渲染
  if (needsRender) {
    render();
  }
}

void FontSelectActivity::handleSelection() {
  Serial.printf("[FONT_SELECT] handleSelection: mode=%d, selectedIndex=%d\n",
                static_cast<int>(mode), selectedIndex);

  if (selectedIndex == 0) {
    // Select Built-in (disable external font)
    if (mode == SelectMode::Reader) {
      Serial.printf("[FONT_SELECT] Disabling reader font\n");
      FontMgr.selectFont(-1);
    } else {
      Serial.printf("[FONT_SELECT] Disabling UI font\n");
      FontMgr.selectUiFont(-1);
    }
  } else {
    // Select external font
    if (mode == SelectMode::Reader) {
      Serial.printf("[FONT_SELECT] Selecting reader font index %d\n", selectedIndex - 1);
      FontMgr.selectFont(selectedIndex - 1);
    } else {
      Serial.printf("[FONT_SELECT] Selecting UI font index %d\n", selectedIndex - 1);
      FontMgr.selectUiFont(selectedIndex - 1);
    }
  }

  Serial.printf("[FONT_SELECT] After selection: readerIndex=%d, uiIndex=%d\n",
                FontMgr.getSelectedIndex(), FontMgr.getUiSelectedIndex());

  // Return to previous page
  onBack();
}

void FontSelectActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  constexpr int rowHeight = 30;

  // Title
  const char *title = (mode == SelectMode::Reader) ? TR(EXT_CHINESE_FONT)
                                                   : TR(EXT_UI_FONT);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true,
                            EpdFontFamily::BOLD);

  // Current selected font marker
  int currentFont = (mode == SelectMode::Reader) ? FontMgr.getSelectedIndex()
                                                 : FontMgr.getUiSelectedIndex();

  // Draw options
  for (int i = 0; i < totalItems && i < 20; i++) { // Max 20 items
    const int itemY = 60 + i * rowHeight;
    const bool isSelected = (i == selectedIndex);
    const bool isCurrent =
        (i == 0 && currentFont < 0) || (i > 0 && i - 1 == currentFont);

    // Draw selection highlight
    if (isSelected) {
      renderer.fillRect(0, itemY - 2, pageWidth - 1, rowHeight);
    }

    // Draw text
    if (i == 0) {
      // Built-in option
      renderer.drawText(UI_10_FONT_ID, 20, itemY, TR(BUILTIN_DISABLED),
                        !isSelected);
    } else {
      // External font
      const FontInfo *info = FontMgr.getFontInfo(i - 1);
      if (info) {
        char label[64];
        snprintf(label, sizeof(label), "%s (%dpt)", info->name, info->size);
        renderer.drawText(UI_10_FONT_ID, 20, itemY, label, !isSelected);
      }
    }

    // Draw current selection marker
    if (isCurrent) {
      const char* marker = TR(ON);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, marker);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, itemY,
                        marker, !isSelected);
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(TR(BACK), TR(SELECT), "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3,
                           labels.btn4);

  renderer.displayBuffer();
}
