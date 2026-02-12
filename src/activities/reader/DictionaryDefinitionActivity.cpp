#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
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

bool DictionaryDefinitionActivity::isLandscape() const {
  return orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CW ||
         orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
}

bool DictionaryDefinitionActivity::isInverted() const {
  return orientation == CrossPointSettings::ORIENTATION::INVERTED;
}

void DictionaryDefinitionActivity::wrapText() {
  wrappedLines.clear();

  const int screenWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(readerFontId);
  constexpr int sidePadding = 20;
  constexpr int topArea = 50;    // Space for title
  constexpr int bottomArea = 40;  // Space for button hints + page indicator
  const int maxWidth = screenWidth - 2 * sidePadding;

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
  const bool landscape = isLandscape();
  const bool inverted = isInverted();

  // Page navigation with orientation-aware button mapping
  bool prevPage, nextPage;
  if (landscape) {
    // Face buttons for page nav (swapped to match physical position) + side buttons
    prevPage = mappedInput.wasReleased(MappedInputManager::Button::Right) ||
               mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up);
    nextPage = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
               mappedInput.wasReleased(MappedInputManager::Button::Down);
  } else if (inverted) {
    // Side buttons swapped (physical up/down are reversed)
    prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
               mappedInput.wasReleased(MappedInputManager::Button::Down);
    nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up);
  } else {
    // Portrait (default)
    prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up);
    nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
               mappedInput.wasReleased(MappedInputManager::Button::Down);
  }

  if (prevPage && currentPage > 0) {
    currentPage--;
    updateRequired = true;
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onBack();
    return;
  }
}

void DictionaryDefinitionActivity::renderScreen() {
  renderer.clearScreen();

  constexpr int sidePadding = 20;
  constexpr int titleY = 10;
  const int lineHeight = renderer.getLineHeight(readerFontId);
  constexpr int bodyStartY = 50;

  // Title: the word in bold (use UI font for title)
  renderer.drawText(UI_12_FONT_ID, sidePadding, titleY, headword.c_str(), true, EpdFontFamily::BOLD);

  // Separator line
  renderer.drawLine(sidePadding, 40, renderer.getScreenWidth() - sidePadding, 40);

  // Body: wrapped definition lines using the same reader font
  int startLine = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLine + i) < static_cast<int>(wrappedLines.size()); i++) {
    int y = bodyStartY + i * lineHeight;
    renderer.drawText(readerFontId, sidePadding, y, wrappedLines[startLine + i].c_str());
  }

  // Pagination indicator on bottom right
  if (totalPages > 1) {
    std::string pageInfo = std::to_string(currentPage + 1) + "/" + std::to_string(totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - sidePadding - textWidth,
                      renderer.getScreenHeight() - 50, pageInfo.c_str());
  }

  // Button hints
  const bool landscape = isLandscape();
  const bool inverted = isInverted();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  if (landscape) {
    // In landscape, drawButtonHints renders text sideways. Draw readable hint line instead.
    std::string hint = "\xC2\xAB Back";
    if (totalPages > 1) {
      hint += "  |  ^ v  |  < >";
    }
    const int hintW = renderer.getTextWidth(SMALL_FONT_ID, hint.c_str());
    renderer.fillRect(0, screenH - 22, screenW, 22, false);
    renderer.drawText(SMALL_FONT_ID, (screenW - hintW) / 2, screenH - 20, hint.c_str());
  } else {
    const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (totalPages > 1) {
      // Side button hints for page navigation (portrait = right edge, inverted = left edge)
      const auto orig = renderer.getOrientation();
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      // Symbols are the same for portrait and inverted: drawSideButtonHints always
      // draws in portrait coords, and button positions + actions both flip, canceling out.
      GUI.drawSideButtonHints(renderer, "^", "v");
      renderer.setOrientation(orig);
    }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
