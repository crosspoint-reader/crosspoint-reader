#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void DictionaryDefinitionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(param);
  self->displayTaskLoop();
}

void DictionaryDefinitionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  wrapText();
  updateRequired = true;
  xTaskCreate(&DictionaryDefinitionActivity::taskTrampoline, "DictDefTask", 4096, this, 1, &displayTaskHandle);
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void DictionaryDefinitionActivity::wrapText() {
  wrappedLines.clear();

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;

  const int screenWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  const int topArea = 50 + hintGutterHeight;
  constexpr int bottomArea = 50;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Process definition text, splitting on \n and word-wrapping
  std::string currentLine;
  std::string currentWord;

  for (size_t i = 0; i <= definition.size(); i++) {
    char c = (i < definition.size()) ? definition[i] : '\0';

    if (c == '\n' || c == '\0') {
      // Flush current word
      if (!currentWord.empty()) {
        if (currentLine.empty()) {
          currentLine = currentWord;
        } else {
          std::string test = currentLine + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLine = test;
          } else {
            wrappedLines.push_back(currentLine);
            currentLine = currentWord;
          }
        }
        currentWord.clear();
      }
      // Flush current line
      wrappedLines.push_back(currentLine);
      currentLine.clear();
    } else if (c == ' ') {
      if (!currentWord.empty()) {
        if (currentLine.empty()) {
          currentLine = currentWord;
        } else {
          std::string test = currentLine + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLine = test;
          } else {
            wrappedLines.push_back(currentLine);
            currentLine = currentWord;
          }
        }
        currentWord.clear();
      }
    } else {
      currentWord += c;
    }
  }

  totalPages = (static_cast<int>(wrappedLines.size()) + linesPerPage - 1) / linesPerPage;
  if (totalPages < 1) totalPages = 1;
}

void DictionaryDefinitionActivity::loop() {
  // Use the same page-turn buttons as the reader (mapped per settings)
  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevPage && currentPage > 0) {
    currentPage--;
    updateRequired = true;
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onDone) {
      onDone();
    } else {
      onBack();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
}

void DictionaryDefinitionActivity::renderScreen() {
  renderer.clearScreen();

  const int titleY = 10 + hintGutterHeight;
  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int separatorY = 40 + hintGutterHeight;
  const int bodyStartY = 50 + hintGutterHeight;

  // Title: the word in bold (use UI font for title)
  renderer.drawText(UI_12_FONT_ID, leftPadding, titleY, headword.c_str(), true, EpdFontFamily::BOLD);

  // Separator line
  renderer.drawLine(leftPadding, separatorY, renderer.getScreenWidth() - rightPadding, separatorY);

  // Body: wrapped definition lines using the same reader font
  int startLine = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLine + i) < static_cast<int>(wrappedLines.size()); i++) {
    int y = bodyStartY + i * lineHeight;
    renderer.drawText(readerFontId, leftPadding, y, wrappedLines[startLine + i].c_str());
  }

  // Pagination indicator on bottom right
  if (totalPages > 1) {
    std::string pageInfo = std::to_string(currentPage + 1) + "/" + std::to_string(totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - 50, pageInfo.c_str());
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", onDone ? "Done" : "", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
