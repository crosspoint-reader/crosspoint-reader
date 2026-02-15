import os

files_to_write = {
    "src/activities/reader/EpubReaderActivity.cpp": r"""#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <sstream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long formattingToggleMs = 500;
// New constant for double click speed
constexpr unsigned long doubleClickMs = 400;

// Global state for the Help Overlay and Night Mode
static bool showHelpOverlay = false;
static bool isNightMode = false;

constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

// Enum for cleaner alignment logic
enum class BoxAlign { LEFT, RIGHT, CENTER };

// Helper to draw multi-line text cleanly
void drawHelpBox(const GfxRenderer& renderer, int x, int y, const char* text, BoxAlign align, int32_t fontId,
                 int lineHeight) {
  // Split text into lines
  std::vector<std::string> lines;
  std::stringstream ss(text);
  std::string line;
  int maxWidth = 0;

  while (std::getline(ss, line, '\n')) {
    lines.push_back(line);
    int w = renderer.getTextWidth(fontId, line.c_str());
    if (w > maxWidth) maxWidth = w;
  }

  // Padding
  int padding = 16;
  int boxWidth = maxWidth + padding;
  int boxHeight = (lines.size() * lineHeight) + padding;

  int drawX = x;
  if (align == BoxAlign::RIGHT) {
    drawX = x - boxWidth;
  } else if (align == BoxAlign::CENTER) {
    drawX = x - (boxWidth / 2);
  }

  // Ensure we don't draw off the bottom edge
  if (y + boxHeight > renderer.getScreenHeight()) {
    y = renderer.getScreenHeight() - boxHeight - 5;
  }

  // Fill White (Clear background)
  renderer.fillRect(drawX, y, boxWidth, boxHeight, false);
  // Draw Border Black (Thickness: 4 for Bold)
  renderer.drawRect(drawX, y, boxWidth, boxHeight, 4, true);

  // Draw each line
  for (size_t i = 0; i < lines.size(); i++) {
    // ALWAYS center text horizontally within the box
    int lineWidth = renderer.getTextWidth(fontId, lines[i].c_str());
    int lineX_centered = drawX + (boxWidth - lineWidth) / 2;

    renderer.drawText(fontId, lineX_centered, y + (padding / 2) + (i * lineHeight), lines[i].c_str());
  }
}

}  // namespace

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Reset help overlay state when entering a book
  showHelpOverlay = false;
  // Reset Night Mode on entry
  isNightMode = false;

  if (!epub) {
    return;
  }

  applyReaderOrientation(renderer, SETTINGS.orientation);

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d\n", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d\n", textSpineIndex);
    }
  }

  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  updateRequired = true;

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask", 8192, this, 1, &displayTaskHandle);
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  // --- POPUP AUTO-DISMISS ---
  static unsigned long clearPopupTimer = 0;
  if (clearPopupTimer > 0 && millis() > clearPopupTimer) {
    clearPopupTimer = 0;
    updateRequired = true;
  }

  // --- HELP OVERLAY INTERCEPTION ---
  if (showHelpOverlay) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      showHelpOverlay = false;
      updateRequired = true;
      return;
    }
    return;
  }

  // --- DOUBLE CLICK STATE VARIABLES ---
  static unsigned long lastFormatDecRelease = 0;
  static bool waitingForFormatDec = false;
  static unsigned long lastFormatIncRelease = 0;
  static bool waitingForFormatInc = false;

  if (subActivity) {
    subActivity->loop();
    if (pendingSubactivityExit) {
      pendingSubactivityExit = false;
      exitActivity();
      updateRequired = true;
      skipNextButtonCheck = true;
    }
    if (pendingGoHome) {
      pendingGoHome = false;
      exitActivity();
      if (onGoHome) {
        onGoHome();
      }
      return;
    }
    return;
  }

  if (pendingGoHome) {
    pendingGoHome = false;
    if (onGoHome) {
      onGoHome();
    }
    return;
  }

  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  // --- CONFIRM BUTTON (MENU / HELP) ---
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL && mappedInput.getHeldTime() > formattingToggleMs) {
      showHelpOverlay = true;
      updateRequired = true;
      return;
    }

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    exitActivity();
    enterNewActivity(new EpubReaderMenuActivity(
        this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
        SETTINGS.orientation, [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
        [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
    xSemaphoreGive(renderingMutex);
  }

  // --- BACK BUTTON LOGIC (Go Home / Dark Mode) ---
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }

  if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL) {
    static unsigned long lastBackRelease = 0;
    static bool waitingForBack = false;

    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
      if (waitingForBack && (millis() - lastBackRelease < doubleClickMs)) {
        waitingForBack = false;
        isNightMode = !isNightMode;
        GUI.drawPopup(renderer, isNightMode ? "Dark Mode" : "Light Mode");
        clearPopupTimer = millis() + 1000;
        updateRequired = true;
        return;
      } else {
        waitingForBack = true;
        lastBackRelease = millis();
      }
    }

    if (waitingForBack && (millis() - lastBackRelease > doubleClickMs)) {
      waitingForBack = false;
      onGoHome();
      return;
    }
  } else {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
      onGoHome();
      return;
    }
  }

  MappedInputManager::Button btnFormatDec;
  MappedInputManager::Button btnFormatInc;
  MappedInputManager::Button btnNavPrev;
  MappedInputManager::Button btnNavNext;

  if (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) {
    if (SETTINGS.swapPortraitControls == 1) {
      btnFormatDec = MappedInputManager::Button::PageBack;
      btnFormatInc = MappedInputManager::Button::PageForward;
      btnNavPrev = MappedInputManager::Button::Left;
      btnNavNext = MappedInputManager::Button::Right;
    } else {
      btnFormatDec = MappedInputManager::Button::Left;
      btnFormatInc = MappedInputManager::Button::Right;
      btnNavPrev = MappedInputManager::Button::PageBack;
      btnNavNext = MappedInputManager::Button::PageForward;
    }
  } else {
    btnFormatDec = MappedInputManager::Button::PageBack;
    btnFormatInc = MappedInputManager::Button::PageForward;
    btnNavPrev = MappedInputManager::Button::Left;
    btnNavNext = MappedInputManager::Button::Right;
  }

  // --- HANDLE FORMAT DEC ---
  bool executeFormatDecSingle = false;

  if (SETTINGS.buttonModMode != CrossPointSettings::MOD_OFF && mappedInput.wasReleased(btnFormatDec)) {
    if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL && mappedInput.getHeldTime() > formattingToggleMs) {
      waitingForFormatDec = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (section) {
        cachedSpineIndex = currentSpineIndex;
        cachedChapterTotalPageCount = section->pageCount;
        nextPageNumber = section->currentPage;
      }
      SETTINGS.lineSpacing++;
      if (SETTINGS.lineSpacing >= CrossPointSettings::LINE_COMPRESSION_COUNT) {
        SETTINGS.lineSpacing = 0;
      }
      const char* spacingMsg = "Spacing: Normal";
      if (SETTINGS.lineSpacing == CrossPointSettings::LINE_COMPRESSION::TIGHT) {
        spacingMsg = "Spacing: Tight";
      } else if (SETTINGS.lineSpacing == CrossPointSettings::LINE_COMPRESSION::WIDE) {
        spacingMsg = "Spacing: Wide";
      }
      SETTINGS.saveToFile();
      section.reset();
      xSemaphoreGive(renderingMutex);
      GUI.drawPopup(renderer, spacingMsg);
      clearPopupTimer = millis() + 1000;
      updateRequired = true;
      return;
    } else {
      if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL && waitingForFormatDec &&
          (millis() - lastFormatDecRelease < doubleClickMs)) {
        waitingForFormatDec = false;
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section) {
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        if (SETTINGS.paragraphAlignment == CrossPointSettings::PARAGRAPH_ALIGNMENT::LEFT_ALIGN) {
          SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::JUSTIFIED;
          GUI.drawPopup(renderer, "Align: Justified");
        } else {
          SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::LEFT_ALIGN;
          GUI.drawPopup(renderer, "Align: Left");
        }
        SETTINGS.saveToFile();
        section.reset();
        xSemaphoreGive(renderingMutex);
        clearPopupTimer = millis() + 1000;
        updateRequired = true;
        return;
      } else {
        if (SETTINGS.buttonModMode == CrossPointSettings::MOD_SIMPLE) {
          executeFormatDecSingle = true;
        } else {
          waitingForFormatDec = true;
          lastFormatDecRelease = millis();
        }
      }
    }
  }

  if (waitingForFormatDec && (millis() - lastFormatDecRelease > doubleClickMs)) {
    waitingForFormatDec = false;
    executeFormatDecSingle = true;
  }

  if (executeFormatDecSingle) {
    bool changed = false;
    bool limitReached = false;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (SETTINGS.fontSize > CrossPointSettings::FONT_SIZE::SMALL) {
      SETTINGS.fontSize--;
      changed = true;
    } else {
      limitReached = true;
    }
    if (changed) {
      if (section) {
        cachedSpineIndex = currentSpineIndex;
        cachedChapterTotalPageCount = section->pageCount;
        nextPageNumber = section->currentPage;
      }
      SETTINGS.saveToFile();
      section.reset();
    }
    xSemaphoreGive(renderingMutex);
    if (changed) {
      updateRequired = true;
    } else if (limitReached) {
      GUI.drawPopup(renderer, "Min Size Reached");
      clearPopupTimer = millis() + 1000;
    }
  }

  // --- HANDLE FORMAT INC ---
  bool executeFormatIncSingle = false;

  if (SETTINGS.buttonModMode != CrossPointSettings::MOD_OFF && mappedInput.wasReleased(btnFormatInc)) {
    if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL && mappedInput.getHeldTime() > formattingToggleMs) {
      waitingForFormatInc = false;
      uint8_t newOrientation = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT)
                                   ? CrossPointSettings::ORIENTATION::LANDSCAPE_CCW
                                   : CrossPointSettings::ORIENTATION::PORTRAIT;
      applyOrientation(newOrientation);
      const char* orientMsg = (newOrientation == CrossPointSettings::ORIENTATION::PORTRAIT) ? "Portrait" : "Landscape";
      GUI.drawPopup(renderer, orientMsg);
      clearPopupTimer = millis() + 1000;
      updateRequired = true;
      return;
    } else {
      if (SETTINGS.buttonModMode == CrossPointSettings::MOD_FULL && waitingForFormatInc &&
          (millis() - lastFormatIncRelease < doubleClickMs)) {
        // DOUBLE CLICK: Toggle Bold (Full Mode Only)
        waitingForFormatInc = false;
        xSemaphoreTake(renderingMutex, portMAX_DELAY);

        if (section) {
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }

        SETTINGS.forceBoldText = (SETTINGS.forceBoldText == 0) ? 1 : 0;
        const char* boldMsg = (SETTINGS.forceBoldText == 1) ? "Bold: ON" : "Bold: OFF";
        SETTINGS.saveToFile();

        // Reset section to force rebuild with proper font metrics
        if (epub) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section ? section->currentPage : 0;
          uint16_t backupPageCount = section ? section->pageCount : 0;

          section.reset();
          saveProgress(backupSpine, backupPage, backupPageCount);
        } else {
          section.reset();
        }

        xSemaphoreGive(renderingMutex);
        GUI.drawPopup(renderer, boldMsg);
        clearPopupTimer = millis() + 1000;
        updateRequired = true;
        return;
      } else {
        if (SETTINGS.buttonModMode == CrossPointSettings::MOD_SIMPLE) {
          executeFormatIncSingle = true;
        } else {
          waitingForFormatInc = true;
          lastFormatIncRelease = millis();
        }
      }
    }
  }

  if (waitingForFormatInc && (millis() - lastFormatIncRelease > doubleClickMs)) {
    waitingForFormatInc = false;
    executeFormatIncSingle = true;
  }

  if (executeFormatIncSingle) {
    bool changed = false;
    bool limitReached = false;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (SETTINGS.fontSize < CrossPointSettings::FONT_SIZE::EXTRA_LARGE) {
      SETTINGS.fontSize++;
      changed = true;
    } else {
      limitReached = true;
    }
    if (changed) {
      if (section) {
        cachedSpineIndex = currentSpineIndex;
        cachedChapterTotalPageCount = section->pageCount;
        nextPageNumber = section->currentPage;
      }
      SETTINGS.saveToFile();
      section.reset();
    }
    xSemaphoreGive(renderingMutex);
    if (changed) {
      updateRequired = true;
    } else if (limitReached) {
      GUI.drawPopup(renderer, "Max Size Reached");
      clearPopupTimer = millis() + 1000;
    }
  }

  // --- HANDLE NAVIGATION BUTTONS ---
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;

  bool prevTriggered = usePressForPageTurn ? mappedInput.wasPressed(btnNavPrev) : mappedInput.wasReleased(btnNavPrev);

  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);

  bool nextTriggered = usePressForPageTurn ? (mappedInput.wasPressed(btnNavNext) || powerPageTurn)
                                           : (mappedInput.wasReleased(btnNavNext) || powerPageTurn);

  if (SETTINGS.buttonModMode == CrossPointSettings::MOD_OFF) {
    if (usePressForPageTurn) {
      if (mappedInput.wasPressed(btnFormatDec)) prevTriggered = true;
      if (mappedInput.wasPressed(btnFormatInc)) nextTriggered = true;
    } else {
      if (mappedInput.wasReleased(btnFormatDec)) prevTriggered = true;
      if (mappedInput.wasReleased(btnFormatInc)) nextTriggered = true;
    }
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
    section.reset();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  if (!section) {
    updateRequired = true;
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentSpineIndex--;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  applyOrientation(orientation);
  updateRequired = true;
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int currentP = section ? section->currentPage : 0;
      const int totalP = section ? section->pageCount : 0;
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentP, totalP,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex) {
            if (currentSpineIndex != newSpineIndex) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubReaderPercentSelectionActivity(
          renderer, mappedInput, initialPercent,
          [this](const int percent) {
            jumpToPercent(percent);
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (epub) {
        uint16_t backupSpine = currentSpineIndex;
        uint16_t backupPage = section->currentPage;
        uint16_t backupPageCount = section->pageCount;

        section.reset();
        epub->clearCache();
        epub->setupCacheDir();

        saveProgress(backupSpine, backupPage, backupPageCount);
      }
      xSemaphoreGive(renderingMutex);
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() { pendingSubactivityExit = true; },
            [this](int newSpineIndex, int newPage) {
              if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
                currentSpineIndex = newSpineIndex;
                nextPageNumber = newPage;
                section.reset();
              }
              pendingSubactivityExit = true;
            }));
        xSemaphoreGive(renderingMutex);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BUTTON_MOD_SETTINGS: {
      SETTINGS.buttonModMode = (SETTINGS.buttonModMode + 1) % CrossPointSettings::BUTTON_MOD_MODE_COUNT;
      SETTINGS.saveToFile();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SWAP_CONTROLS: {
      SETTINGS.swapPortraitControls = (SETTINGS.swapPortraitControls == 0) ? 1 : 0;
      SETTINGS.saveToFile();
      pendingSubactivityExit = true;
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();

  applyReaderOrientation(renderer, SETTINGS.orientation);

  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::displayTaskLoop() {
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

void EpubReaderActivity::renderScreen() {
  if (!epub) {
    return;
  }

  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  auto metrics = UITheme::getInstance().getMetrics();

  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - SETTINGS.screenMargin +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d\n", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.forceBoldText)) {
      LOG_DBG("ERS", "Cache not found, building...\n");

      const auto popupFn = [this]() { GUI.drawPopup(renderer, "Indexing..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      popupFn, SETTINGS.forceBoldText)) {
        LOG_ERR("ERS", "Failed to persist page data to SD\n");
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...\n");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (cachedChapterTotalPageCount > 0) {
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render\n");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)\n", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache\n");
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms\n", millis() - start);
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d\n", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!\n");
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  
  // Render clean text utilizing True Bold parameter
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, SETTINGS.forceBoldText);

  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  // Apply dark mode filter globally if active
  if (isNightMode) {
    renderer.invertScreen();
  }

  // --- HELP OVERLAY RENDERING ---
  if (showHelpOverlay) {
    const int w = renderer.getScreenWidth();
    const int h = renderer.getScreenHeight();

    int32_t overlayFontId = SMALL_FONT_ID;
    int overlayLineHeight = 18;

    int dismissY = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) ? 500 : 300;
    int dismissX = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) ? w / 2 : w / 2 + 25;

    drawHelpBox(renderer, dismissX, dismissY,
                "PRESS ANY KEY\n"
                "TO DISMISS",
                BoxAlign::CENTER, overlayFontId, overlayLineHeight);

    if (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) {
      if (SETTINGS.swapPortraitControls == 1) {
        drawHelpBox(renderer, 10, h - 80, "2x: Dark", BoxAlign::LEFT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h / 2 - 70,
                    "1x: Text size –\n"
                    "Hold: Spacing\n"
                    "2x: Alignment",
                    BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h / 2 + 10,
                    "1x: Text size +\n"
                    "Hold: Rotate\n"
                    "2x: Bold",
                    BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
      } else {
        drawHelpBox(renderer, 10, h - 80, "2x: Dark", BoxAlign::LEFT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 145, h - 80,
                    "1x: Text size –\n"
                    "Hold: Spacing\n"
                    "2x: Alignment",
                    BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h - 80,
                    "1x: Text size +\n"
                    "Hold: Rotate\n"
                    "2x: Bold",
                    BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
      }
    } else {
      drawHelpBox(renderer, w - 10, h - 40, "2x: Dark", BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
      drawHelpBox(renderer, w / 2 + 20, 20,
                  "1x: Text size –\n"
                  "Hold: Spacing\n"
                  "2x: Alignment",
                  BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
      drawHelpBox(renderer, w / 2 + 30, 20,
                  "1x: Text size +\n"
                  "Hold: Rotate\n"
                  "2x: Bold",
                  BoxAlign::LEFT, overlayFontId, overlayLineHeight);
    }
  }

  // --- STANDARD REFRESH ---
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  renderer.storeBwBuffer();

  if (SETTINGS.textAntiAliasing && !showHelpOverlay && !isNightMode) {  // Don't anti-alias the help overlay
    renderer.clearScreen(0x00);

    // --- LSB (Light Grays) Pass ---
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, SETTINGS.forceBoldText);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);

    // --- MSB (Dark Grays) Pass ---
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, SETTINGS.forceBoldText);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];

    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showBookProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(bookProgress));
  }

  if (showChapterProgressBar) {
    const float chapterProgress =
        (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100 : 0;
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(chapterProgress));
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showChapterTitle) {
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + 30;

    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (tocIndex == -1) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}

}
""",
    "src/CrossPointSettings.h": r"""#pragma once
#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  CrossPointSettings() = default;
  static CrossPointSettings instance;

 public:
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum ORIENTATION { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3, ORIENTATION_COUNT };
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTON_LAYOUT_COUNT };
  enum BUTTON_MOD_MODE { MOD_OFF = 0, MOD_SIMPLE = 1, MOD_FULL = 2, BUTTON_MOD_MODE_COUNT };
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, OPENDYSLEXIC = 2, FONT_FAMILY_COUNT };
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, SHORT_PWRBTN_COUNT };
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };
  enum UI_THEME { CLASSIC = 0, LYRA = 1 };

  uint8_t sleepScreen = DARK;
  uint8_t sleepScreenCoverMode = FIT;
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  uint8_t statusBar = FULL;
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  uint8_t shortPwrBtn = IGNORE;
  uint8_t orientation = PORTRAIT;
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  uint8_t fontFamily = BOOKERLY;
  uint8_t fontSize = MEDIUM;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  uint8_t sleepTimeout = SLEEP_10_MIN;
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;
  uint8_t screenMargin = 5;
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  char blePageTurnerMac[18] = "";
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  uint8_t longPressChapterSkip = 1;
  uint8_t uiTheme = LYRA;
  uint8_t fadingFix = 0;
  uint8_t embeddedStyle = 1;
  uint8_t buttonModMode = MOD_FULL;
  uint8_t forceBoldText = 0;
  uint8_t swapPortraitControls = 0;

  ~CrossPointSettings() = default;

  static CrossPointSettings& getInstance() { return instance; }

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;
  bool saveToFile() const;
  bool loadFromFile();
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

#define SETTINGS CrossPointSettings::getInstance()
""",
    "src/CrossPointSettings.cpp": r"""#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
// Increment this when adding new persisted settings fields
constexpr uint8_t SETTINGS_COUNT = 33;
constexpr char SETTINGS_FILE[] = "/.crosspoint/settings.bin";

// Validate front button mapping to ensure each hardware button is unique.
// If duplicates are detected, reset to the default physical order to prevent invalid mappings.
void validateFrontButtonMapping(CrossPointSettings& settings) {
  // Snapshot the logical->hardware mapping so we can compare for duplicates.
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        // Duplicate detected: restore the default physical order (Back, Confirm, Left, Right).
        settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
        settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
        settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
        settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}
}  // namespace

bool CrossPointSettings::saveToFile() const {
  // Make sure the directory exists
  Storage.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!Storage.openFileForWrite("CPS", SETTINGS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, statusBar);
  serialization::writePod(outputFile, orientation);
  serialization::writePod(outputFile, frontButtonLayout);  // legacy
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, fontFamily);
  serialization::writePod(outputFile, fontSize);
  serialization::writePod(outputFile, lineSpacing);
  serialization::writePod(outputFile, paragraphAlignment);
  serialization::writePod(outputFile, sleepTimeout);
  serialization::writePod(outputFile, refreshFrequency);
  serialization::writePod(outputFile, screenMargin);
  serialization::writePod(outputFile, sleepScreenCoverMode);
  serialization::writeString(outputFile, std::string(opdsServerUrl));
  serialization::writePod(outputFile, textAntiAliasing);
  serialization::writePod(outputFile, hideBatteryPercentage);
  serialization::writePod(outputFile, longPressChapterSkip);
  serialization::writePod(outputFile, hyphenationEnabled);
  serialization::writeString(outputFile, std::string(opdsUsername));
  serialization::writeString(outputFile, std::string(opdsPassword));
  serialization::writePod(outputFile, sleepScreenCoverFilter);
  serialization::writePod(outputFile, uiTheme);
  serialization::writePod(outputFile, frontButtonBack);
  serialization::writePod(outputFile, frontButtonConfirm);
  serialization::writePod(outputFile, frontButtonLeft);
  serialization::writePod(outputFile, frontButtonRight);
  serialization::writePod(outputFile, fadingFix);
  serialization::writePod(outputFile, embeddedStyle);
  serialization::writePod(outputFile, forceBoldText);
  serialization::writePod(outputFile, buttonModMode);
  serialization::writePod(outputFile, swapPortraitControls);
  // New fields added at end for backward compatibility
  outputFile.close();

  LOG_DBG("CPS", "Settings saved to file");
  return true;
}

bool CrossPointSettings::loadFromFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // load settings that exist (support older files with fewer fields)
  uint8_t settingsRead = 0;
  // Track whether remap fields were present in the settings file.
  bool frontButtonMappingRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, forceBoldText);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, buttonModMode, BUTTON_MOD_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, swapPortraitControls);
    if (++settingsRead >= fileSettingsCount) break;
    // New fields added at end for backward compatibility
  } while (false);

  if (frontButtonMappingRead) {
    validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  inputFile.close();
  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case OPENDYSLEXIC:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (fontSize) {
        case SMALL:
          return BOOKERLY_12_FONT_ID;
        case MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case LARGE:
          return BOOKERLY_16_FONT_ID;
        case EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
    case NOTOSANS:
      switch (fontSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case OPENDYSLEXIC:
      switch (fontSize) {
        case SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
  }
}
""",
    "src/MappedInputManager.cpp": r"""#include "MappedInputManager.h"

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);

  // Lock the side button swap to portrait orientation only.
  // In landscape modes, we enforce the default physical layout to prevent awkward top/bottom page turning.
  if (SETTINGS.orientation != CrossPointSettings::PORTRAIT) {
    sideLayout = CrossPointSettings::PREV_NEXT;
  }

  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return previous;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
""",
    "src/SettingsList.h": r"""#pragma once

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen,
                        {"Dark", "Light", "Custom", "Cover", "None", "Cover + Custom"}, "sleepScreen", "Display"),
      SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"},
                        "sleepScreenCoverMode", "Display"),
      SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                        {"None", "Contrast", "Inverted"}, "sleepScreenCoverFilter", "Display"),
      SettingInfo::Enum(
          "Status Bar", &CrossPointSettings::statusBar,
          {"None", "No Progress", "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar"},
          "statusBar", "Display"),
      SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"},
                        "hideBatteryPercentage", "Display"),
      SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                        {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}, "refreshFrequency", "Display"),
      SettingInfo::Enum("UI Theme", &CrossPointSettings::uiTheme, {"Classic", "Lyra"}, "uiTheme", "Display"),
      SettingInfo::Toggle("Sunlight Fading Fix", &CrossPointSettings::fadingFix, "fadingFix", "Display"),

      // --- Reader ---
      SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic"},
                        "fontFamily", "Reader"),
      SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}, "fontSize",
                        "Reader"),
      SettingInfo::Toggle("Force Bold Text", &CrossPointSettings::forceBoldText, "forceBoldText", "Reader"),
      SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}, "lineSpacing",
                        "Reader"),
      SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin", "Reader"),
      SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                        {"Justify", "Left", "Center", "Right", "Book's Style"}, "paragraphAlignment", "Reader"),
      SettingInfo::Toggle("Book's Embedded Style", &CrossPointSettings::embeddedStyle, "embeddedStyle", "Reader"),
      SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled", "Reader"),
      SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                        {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}, "orientation", "Reader"),
      SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing,
                          "extraParagraphSpacing", "Reader"),
      SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing, "textAntiAliasing", "Reader"),

      // --- Controls ---
      SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                        {"Prev, Next", "Next, Prev"}, "sideButtonLayout", "Controls"),
      SettingInfo::Enum("Button Mod", &CrossPointSettings::buttonModMode, {"Off", "Simple", "Full"}, "buttonModMode",
                        "Controls"),
      SettingInfo::Toggle("Swap Portrait Controls", &CrossPointSettings::swapPortraitControls, "swapPortraitControls",
                          "Controls"),
      SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          "Controls"),
      SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn, {"Ignore", "Sleep", "Page Turn"},
                        "shortPwrBtn", "Controls"),

      // --- System ---
      SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                        {"1 min", "5 min", "10 min", "15 min", "30 min"}, "sleepTimeout", "System"),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          "KOReader Username", [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", "KOReader Sync"),
      SettingInfo::DynamicString(
          "KOReader Password", [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", "KOReader Sync"),
      SettingInfo::DynamicString(
          "Sync Server URL", [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", "KOReader Sync"),
      SettingInfo::DynamicEnum(
          "Document Matching", {"Filename", "Binary"},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", "KOReader Sync"),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String("OPDS Server URL", SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl), "opdsServerUrl",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Username", SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Password", SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          "OPDS Browser"),
  };
}
"""
}

for path, content in files_to_write.items():
    # Make sure directories exist
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content.strip() + '\n')
    print(f"Updated {path}")

print("\nAll files successfully written!")
