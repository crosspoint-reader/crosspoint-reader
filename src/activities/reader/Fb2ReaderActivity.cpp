#include "Fb2ReaderActivity.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "Fb2ReaderChapterSelectionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;

int clampPercent(int percent) {
  if (percent < 0) return 0;
  if (percent > 100) return 100;
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
}  // namespace

void Fb2ReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<Fb2ReaderActivity*>(param);
  self->displayTaskLoop();
}

void Fb2ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!fb2) return;

  applyReaderOrientation(renderer, SETTINGS.orientation);
  renderingMutex = xSemaphoreCreateMutex();
  fb2->setupCacheDir();

  // Load progress
  FsFile f;
  if (Storage.openFileForRead("FBR", fb2->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSectionIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSectionIndex = currentSectionIndex;
    }
    if (dataSize == 6) {
      cachedSectionTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }

  APP_STATE.openEpubPath = fb2->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(fb2->getPath(), fb2->getTitle(), fb2->getAuthor(), fb2->getThumbBmpPath());

  updateRequired = true;
  xTaskCreate(&Fb2ReaderActivity::taskTrampoline, "Fb2ReaderTask", 8192, this, 1, &displayTaskHandle);
}

void Fb2ReaderActivity::onExit() {
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
  fb2.reset();
}

void Fb2ReaderActivity::loop() {
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
      if (onGoHome) onGoHome();
      return;
    }
    return;
  }

  if (pendingGoHome) {
    pendingGoHome = false;
    if (onGoHome) onGoHome();
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

  // Menu
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (fb2 && fb2->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = fb2->calculateProgress(currentSectionIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    exitActivity();
    enterNewActivity(new EpubReaderMenuActivity(
        this->renderer, this->mappedInput, fb2->getTitle(), currentPage, totalPages, bookProgressPercent,
        SETTINGS.orientation, [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
        [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
    xSemaphoreGive(renderingMutex);
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }

  // Short press BACK goes home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) return;

  // End of book handling
  if (currentSectionIndex > 0 && currentSectionIndex >= fb2->getSectionCount()) {
    currentSectionIndex = fb2->getSectionCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    const int nextIndex = nextTriggered ? currentSectionIndex + 1 : currentSectionIndex - 1;
    if (nextIndex < 0) return;  // Already at the beginning of the book
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSectionIndex = nextIndex;
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
    } else if (currentSectionIndex > 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentSectionIndex--;
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
      currentSectionIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

void Fb2ReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  applyOrientation(orientation);
  updateRequired = true;
}

void Fb2ReaderActivity::jumpToPercent(int percent) {
  if (!fb2) return;

  const size_t bookSize = fb2->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int sectionCount = fb2->getSectionCount();
  if (sectionCount == 0) return;

  int targetIdx = sectionCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < sectionCount; i++) {
    const size_t cumulative = fb2->getCumulativeSectionSize(i);
    if (targetSize <= cumulative) {
      targetIdx = i;
      prevCumulative = (i > 0) ? fb2->getCumulativeSectionSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = fb2->getCumulativeSectionSize(targetIdx);
  const size_t sectionSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (sectionSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(sectionSize);
  if (pendingSpineProgress < 0.0f) pendingSpineProgress = 0.0f;
  if (pendingSpineProgress > 1.0f) pendingSpineProgress = 1.0f;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  currentSectionIndex = targetIdx;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void Fb2ReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int currentP = section ? section->currentPage : 0;
      const int totalP = section ? section->pageCount : 0;
      const int secIdx = currentSectionIndex;

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new Fb2ReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, fb2, secIdx, currentP, totalP,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSectionIndex) {
            if (currentSectionIndex != newSectionIndex) {
              currentSectionIndex = newSectionIndex;
              nextPageNumber = 0;
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
      if (fb2 && fb2->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = fb2->calculateProgress(currentSectionIndex, chapterProgress) * 100.0f;
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
      if (fb2) {
        uint16_t backupSection = currentSectionIndex;
        uint16_t backupPage = section ? section->currentPage : 0;
        uint16_t backupPageCount = section ? section->pageCount : 0;

        section.reset();
        fb2->clearCache();
        fb2->setupCacheDir();
        saveProgress(backupSection, backupPage, backupPageCount);
      }
      xSemaphoreGive(renderingMutex);
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      // KOReader sync not supported for FB2
      break;
    }
    default:
      break;
  }
}

void Fb2ReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) return;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (section) {
    cachedSectionIndex = currentSectionIndex;
    cachedSectionTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  applyReaderOrientation(renderer, SETTINGS.orientation);
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void Fb2ReaderActivity::displayTaskLoop() {
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

void Fb2ReaderActivity::renderScreen() {
  if (!fb2) return;

  if (currentSectionIndex < 0) currentSectionIndex = 0;
  if (currentSectionIndex > fb2->getSectionCount()) currentSectionIndex = fb2->getSectionCount();

  if (currentSectionIndex == fb2->getSectionCount()) {
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
    const auto& sectionInfo = fb2->getSectionInfo(currentSectionIndex);
    Serial.printf("[%lu] [FBR] Loading section %d: %s\n", millis(), currentSectionIndex, sectionInfo.title.c_str());
    section = std::unique_ptr<Fb2Section>(new Fb2Section(fb2, currentSectionIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled)) {
      Serial.printf("[%lu] [FBR] Cache not found, building...\n", millis());
      const auto popupFn = [this]() { GUI.drawPopup(renderer, "Indexing..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, popupFn)) {
        Serial.printf("[%lu] [FBR] Failed to build section\n", millis());
        section.reset();
        return;
      }
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (cachedSectionTotalPageCount > 0) {
      if (currentSectionIndex == cachedSectionIndex && section->pageCount != cachedSectionTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedSectionTotalPageCount);
        section->currentPage = static_cast<int>(progress * section->pageCount);
      }
      cachedSectionTotalPageCount = 0;
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  }
  saveProgress(currentSectionIndex, section->currentPage, section->pageCount);
}

void Fb2ReaderActivity::saveProgress(int sectionIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("FBR", fb2->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = sectionIndex & 0xFF;
    data[1] = (sectionIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
  }
}

void Fb2ReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
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

void Fb2ReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
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

  const float sectionChapterProg =
      (section->pageCount > 0) ? static_cast<float>(section->currentPage + 1) / section->pageCount : 0;
  const float bookProgress = fb2->calculateProgress(currentSectionIndex, sectionChapterProg) * 100;

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
    const int tocIndex = fb2->getTocIndexForSectionIndex(currentSectionIndex);

    std::string sectionTitle;
    int titleWidth;
    if (tocIndex == -1) {
      sectionTitle = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto& tocEntry = fb2->getTocEntry(tocIndex);
      sectionTitle = tocEntry.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, sectionTitle.c_str());
      if (titleWidth > availableTitleSpace) {
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        sectionTitle = renderer.truncatedText(SMALL_FONT_ID, sectionTitle.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, sectionTitle.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      sectionTitle.c_str());
  }
}
