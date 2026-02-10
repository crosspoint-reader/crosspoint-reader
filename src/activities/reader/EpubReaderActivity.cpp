#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

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
constexpr unsigned long doubleClickMs = 300;

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

// Apply the logical reader orientation to the renderer.
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

}  // namespace

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

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
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
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
      Serial.printf("[%lu] [ERS] Opened for first time, navigating to text reference at index %d\n", millis(),
                    textSpineIndex);
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
  // --- POPUP AUTO-DISMISS LOGIC ---
  static unsigned long clearPopupTimer = 0;
  if (clearPopupTimer > 0 && millis() > clearPopupTimer) {
    clearPopupTimer = 0;
    updateRequired = true;
  }

  // --- DOUBLE CLICK STATE ---
  static unsigned long lastFormatDecRelease = 0;
  static bool waitingForFormatDec = false;
  static unsigned long lastFormatIncRelease = 0;
  static bool waitingForFormatInc = false;

  // Pass input responsibility to sub activity if exists
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

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // =========================================================================================
  // DYNAMIC BUTTON MAPPING LOGIC
  // =========================================================================================

  MappedInputManager::Button btnFormatDec;
  MappedInputManager::Button btnFormatInc;
  MappedInputManager::Button btnNavPrev;
  MappedInputManager::Button btnNavNext;

  if (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) {
    btnFormatDec = MappedInputManager::Button::Left;
    btnFormatInc = MappedInputManager::Button::Right;
    btnNavPrev = MappedInputManager::Button::PageBack;
    btnNavNext = MappedInputManager::Button::PageForward;
  } else {
    btnFormatDec = MappedInputManager::Button::PageBack;
    btnFormatInc = MappedInputManager::Button::PageForward;
    btnNavPrev = MappedInputManager::Button::Left;
    btnNavNext = MappedInputManager::Button::Right;
  }

  // --- HANDLE FORMAT DEC (Left/Top) ---
  // Actions: Short (Size-), Long (Spacing), Double (Align)

  bool executeFormatDecSingle = false;

  if (mappedInput.wasReleased(btnFormatDec)) {
    if (mappedInput.getHeldTime() > formattingToggleMs) {
      // Long Press: Cycle Spacing (Immediate)
      waitingForFormatDec = false;  // Cancel any pending
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
      // Short Press detected - check if it's a double click
      if (waitingForFormatDec && (millis() - lastFormatDecRelease < doubleClickMs)) {
        // DOUBLE CLICK: Toggle Alignment
        waitingForFormatDec = false;
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section) {
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        // Toggle between Left and Justified
        if (SETTINGS.paragraphAlignment == CrossPointSettings::PARAGRAPH_ALIGNMENT::LEFT) {
          SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::JUSTIFIED;
          GUI.drawPopup(renderer, "Align: Justified");
        } else {
          SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::LEFT;
          GUI.drawPopup(renderer, "Align: Left");
        }
        SETTINGS.saveToFile();
        section.reset();
        xSemaphoreGive(renderingMutex);
        clearPopupTimer = millis() + 1000;
        updateRequired = true;
        return;
      } else {
        // First click - start waiting
        waitingForFormatDec = true;
        lastFormatDecRelease = millis();
      }
    }
  }

  // Check for timeout on pending single click
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

  // --- HANDLE FORMAT INC (Right/Top) ---
  // Actions: Short (Size+), Long (Rotate), Double (Anti-Alias)

  bool executeFormatIncSingle = false;

  if (mappedInput.wasReleased(btnFormatInc)) {
    if (mappedInput.getHeldTime() > formattingToggleMs) {
      // Long Press: Toggle Orientation (Immediate)
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
      // Short Press detected
      if (waitingForFormatInc && (millis() - lastFormatIncRelease < doubleClickMs)) {
        // DOUBLE CLICK: Toggle Anti-Aliasing
        waitingForFormatInc = false;
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section) {
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        SETTINGS.textAntiAliasing = !SETTINGS.textAntiAliasing;
        const char* aaMsg = SETTINGS.textAntiAliasing ? "Anti-Alias: ON" : "Anti-Alias: OFF";
        SETTINGS.saveToFile();
        section.reset();
        xSemaphoreGive(renderingMutex);
        GUI.drawPopup(renderer, aaMsg);
        clearPopupTimer = millis() + 1000;
        updateRequired = true;
        return;
      } else {
        // First click
        waitingForFormatInc = true;
        lastFormatIncRelease = millis();
      }
    }
  }

  // Check for timeout
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

  // Logic to detect navigation trigger based on current mapping
  const bool prevTriggered =
      usePressForPageTurn ? mappedInput.wasPressed(btnNavPrev) : mappedInput.wasReleased(btnNavPrev);

  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);

  const bool nextTriggered = usePressForPageTurn ? (mappedInput.wasPressed(btnNavNext) || powerPageTurn)
                                                 : (mappedInput.wasReleased(btnNavNext) || powerPageTurn);

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool isHoldingNav = mappedInput.isPressed(btnNavPrev) || mappedInput.isPressed(btnNavNext);
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

// TODO: Failure handling
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
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      const auto popupFn = [this]() { GUI.drawPopup(renderer, "Indexing..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, popupFn)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
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
    Serial.printf("[%lu] [ERS] No pages to render\n", millis());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
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
    Serial.printf("[ERS] Progress saved: Chapter %d, Page %d\n", spineIndex, currentPage);
  } else {
    Serial.printf("[ERS] Could not save progress!\n");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  renderer.storeBwBuffer();

  if (SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
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
