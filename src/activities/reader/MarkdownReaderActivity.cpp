#include "MarkdownReaderActivity.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <MarkdownRenderer.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "SpiBusMutex.h"
#include "TocActivity.h"
#include "activities/TaskShutdown.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;
}  // namespace

void MarkdownReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!markdown) {
    return;
  }

  switch (SETTINGS.orientation) {
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

  markdown->setupCacheDir();
  // Parse AST for Obsidian rendering and navigation (best effort)
  astReady.store(markdown->parseToAst());
  useAstRenderer.store(astReady.load());
  if (!useAstRenderer.load()) {
    htmlReady.store(markdown->ensureHtml());
  }

  APP_STATE.openEpubPath = markdown->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(markdown->getPath(), markdown->getTitle(), "", "");

  loadProgress();
  requestUpdate();
}

void MarkdownReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  mdSection.reset();
  htmlSection.reset();
  markdown.reset();
}

void MarkdownReaderActivity::loop() {
  ActivityWithSubactivity::loop();
  if (subActivity) {
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    if (onBackHomeFn != nullptr) onBackHomeFn(callbackCtx);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    if (onBackToLibraryFn != nullptr) onBackToLibraryFn(callbackCtx, markdown->getPath());
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showTableOfContents();
    return;
  }

  if (isRenderPending()) {
    return;
  }

  // Long press for heading navigation (when enabled and AST is available)
  if (SETTINGS.longPressChapterSkip && astReady.load()) {
    constexpr unsigned long headingSkipMs = 500;
    const bool leftHeld = mappedInput.isPressed(MappedInputManager::Button::Left) ||
                          mappedInput.isPressed(MappedInputManager::Button::PageBack);
    const bool rightHeld = mappedInput.isPressed(MappedInputManager::Button::Right) ||
                           mappedInput.isPressed(MappedInputManager::Button::PageForward);

    if (leftHeld && mappedInput.getHeldTime() >= headingSkipMs) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
          mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        jumpToPrevHeading();
        return;
      }
    }

    if (rightHeld && mappedInput.getHeldTime() >= headingSkipMs) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
          mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        jumpToNextHeading();
        return;
      }
    }
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

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (!hasActiveSection() || getActivePageCount() == 0) {
    requestUpdate();
    return;
  }

  const int currentPage = getActiveCurrentPage();
  const uint16_t pageCount = getActivePageCount();

  if (prevTriggered && currentPage > 0) {
    setActiveCurrentPage(currentPage - 1);
    requestUpdate();
  } else if (nextTriggered && currentPage < static_cast<int>(pageCount - 1)) {
    setActiveCurrentPage(currentPage + 1);
    requestUpdate();
  }
}

void MarkdownReaderActivity::render(Activity::RenderLock&& lock) { renderScreen(); }

void MarkdownReaderActivity::renderScreen() {
  if (!markdown) {
    return;
  }

  if (useAstRenderer.load()) {
    if (!astReady.load() || !markdown->getAst()) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, "Markdown error", true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
  } else if (!htmlReady.load()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Markdown error", true, EpdFontFamily::BOLD);
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

  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - SETTINGS.screenMargin +
                            (showProgressBar ? (ScreenComponents::BOOK_PROGRESS_BAR_HEIGHT + progressBarMarginTop) : 0);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  bool sectionInitialized = false;

  auto progressSetup = [this] {
    constexpr int barWidth = 200;
    constexpr int barHeight = 10;
    constexpr int boxMargin = 20;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, "Indexing...");
    const int boxWidthWithBar = (barWidth > textWidth ? barWidth : textWidth) + boxMargin * 2;
    const int boxHeightWithBar = renderer.getLineHeight(UI_12_FONT_ID) + barHeight + boxMargin * 3;
    const int boxXWithBar = (renderer.getScreenWidth() - boxWidthWithBar) / 2;
    constexpr int boxY = 50;
    const int barX = boxXWithBar + (boxWidthWithBar - barWidth) / 2;
    const int barY = boxY + renderer.getLineHeight(UI_12_FONT_ID) + boxMargin * 2;

    renderer.fillRect(boxXWithBar, boxY, boxWidthWithBar, boxHeightWithBar, false);
    renderer.drawText(UI_12_FONT_ID, boxXWithBar + boxMargin, boxY + boxMargin, "Indexing...");
    renderer.drawRect(boxXWithBar + 5, boxY + 5, boxWidthWithBar - 10, boxHeightWithBar - 10);
    renderer.drawRect(barX, barY, barWidth, barHeight);
    renderer.displayBuffer();
  };

  auto progressCallback = [this](int progress) {
    constexpr int barWidth = 200;
    constexpr int barHeight = 10;
    constexpr int boxMargin = 20;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, "Indexing...");
    const int boxWidthWithBar = (barWidth > textWidth ? barWidth : textWidth) + boxMargin * 2;
    const int boxXWithBar = (renderer.getScreenWidth() - boxWidthWithBar) / 2;
    constexpr int boxY = 50;
    const int barX = boxXWithBar + (boxWidthWithBar - barWidth) / 2;
    const int barY = boxY + renderer.getLineHeight(UI_12_FONT_ID) + boxMargin * 2;

    const int fillWidth = (barWidth - 2) * progress / 100;
    renderer.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, true);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  if (useAstRenderer.load()) {
    if (!mdSection) {
      sectionInitialized = true;
      mdSection.reset(new MarkdownSection(markdown->getCachePath(), markdown->getContentBasePath(), renderer));

      bool sectionLoaded = false;
      {
        SpiBusMutex::Guard guard;
        sectionLoaded = mdSection->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                                   SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment,
                                                   viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                                   static_cast<uint32_t>(markdown->getFileSize()));
      }

      if (!sectionLoaded) {
        renderer.fillRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), false);
        renderer.displayBuffer();
        pagesUntilFullRefresh = 0;

        if (!mdSection->createSectionFile(*markdown->getAst(), SETTINGS.getReaderFontId(),
                                          SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                          SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                          SETTINGS.hyphenationEnabled, static_cast<uint32_t>(markdown->getFileSize()),
                                          progressSetup, progressCallback)) {
          LOG_ERR("MDR", "Failed to build markdown AST cache, falling back to HTML");
          mdSection.reset();
          useAstRenderer.store(false);
        }
      }
    }

    if (useAstRenderer.load() && mdSection) {
      if (hasSavedPage) {
        if (mdSection->pageCount > 0) {
          mdSection->currentPage = std::min(savedPage, static_cast<int>(mdSection->pageCount - 1));
        } else {
          mdSection->currentPage = 0;
        }
        hasSavedPage = false;
      }

      if (sectionInitialized && astReady.load()) {
        auto* nav = markdown->getNavigation();
        if (nav) {
          if (mdSection->hasNodeToPageMap()) {
            nav->updatePageNumbers(mdSection->getNodeToPageMap());
          } else {
            MarkdownRenderer mdRenderer(renderer, SETTINGS.getReaderFontId(), viewportWidth, viewportHeight,
                                        SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                        SETTINGS.paragraphAlignment, SETTINGS.hyphenationEnabled,
                                        markdown->getContentBasePath());
            {
              SpiBusMutex::Guard guard;
              mdRenderer.render(*markdown->getAst(), [](std::unique_ptr<Page> page) { (void)page; });
            }
            nav->updatePageNumbers(mdRenderer.getNodeToPageMap());
          }
        }
      }
    }
  }

  if (!useAstRenderer.load()) {
    if (!htmlReady.load()) {
      htmlReady.store(markdown->ensureHtml());
      if (!htmlReady.load()) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, "Markdown error", true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        return;
      }
    }

    if (!htmlSection) {
      sectionInitialized = true;
      htmlSection.reset(
          new HtmlSection(markdown->getHtmlPath(), markdown->getCachePath(), markdown->getContentBasePath(), renderer));

      bool sectionLoaded = false;
      {
        SpiBusMutex::Guard guard;
        sectionLoaded = htmlSection->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment,
                                                     viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                                     static_cast<uint32_t>(markdown->getFileSize()));
      }

      if (!sectionLoaded) {
        renderer.fillRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), false);
        renderer.displayBuffer();
        pagesUntilFullRefresh = 0;

        if (!htmlSection->createSectionFile(
                SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                static_cast<uint32_t>(markdown->getFileSize()), progressSetup, progressCallback)) {
          LOG_ERR("MDR", "Failed to build markdown cache");
          htmlSection.reset();
          return;
        }
      }

      if (hasSavedPage) {
        if (htmlSection->pageCount > 0) {
          htmlSection->currentPage = std::min(savedPage, static_cast<int>(htmlSection->pageCount - 1));
        } else {
          htmlSection->currentPage = 0;
        }
        hasSavedPage = false;
      }

      if (sectionInitialized && astReady.load() && markdown->getAst()) {
        auto* nav = markdown->getNavigation();
        if (nav) {
          MarkdownRenderer mdRenderer(renderer, SETTINGS.getReaderFontId(), viewportWidth, viewportHeight,
                                      SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                      SETTINGS.paragraphAlignment, SETTINGS.hyphenationEnabled,
                                      markdown->getContentBasePath());
          // Full render is required to compute accurate node->page mapping based on layout.
          {
            SpiBusMutex::Guard guard;
            mdRenderer.render(*markdown->getAst(), [](std::unique_ptr<Page> page) { (void)page; });
          }
          nav->updatePageNumbers(mdRenderer.getNodeToPageMap());
        }
      }
    }
  }

  renderer.clearScreen();

  if (!hasActiveSection() || getActivePageCount() == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty document", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (getActiveCurrentPage() < 0 || getActiveCurrentPage() >= static_cast<int>(getActivePageCount())) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  std::unique_ptr<Page> page;
  {
    SpiBusMutex::Guard guard;
    page = loadActivePage();
  }

  if (!page) {
    if (useAstRenderer.load() && mdSection) {
      mdSection->clearCache();
      mdSection.reset();
    } else if (htmlSection) {
      htmlSection->clearCache();
      htmlSection.reset();
    }
    return;
  }

  renderContents(std::move(page), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  saveProgress();
}

void MarkdownReaderActivity::renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                                            int orientedMarginBottom, int orientedMarginLeft) {
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

  // Grayscale antialiasing is skipped in dark mode for the same reason as EpubReaderActivity:
  // the EPD grayscale LUT is polarity-dependent and produces ghosting after a dark-mode BW refresh.
  if (SETTINGS.textAntiAliasing && !renderer.isDarkMode()) {
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

void MarkdownReaderActivity::renderStatusBar(int orientedMarginRight, int orientedMarginBottom,
                                             int orientedMarginLeft) const {
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  float progress = 0.0f;
  const uint16_t pageCount = getActivePageCount();
  const int currentPage = getActiveCurrentPage();
  const int displayPage = (pageCount > 0 && currentPage >= 0) ? (currentPage + 1) : 0;
  if (pageCount > 0 && currentPage >= 0) {
    progress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount) * 100.0f;
  }

  if (showProgressText || showProgressPercentage) {
    char progressStr[32];
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", displayPage, pageCount, progress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", displayPage, pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showProgressBar) {
    ScreenComponents::drawBookProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showBattery) {
    ScreenComponents::drawBattery(renderer, orientedMarginLeft + 1, textY, showBatteryPercentage);
  }

  if (showTitle && markdown) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;

    std::string title = markdown->getTitle();
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    while (titleWidth > availableTextWidth && title.length() > 11) {
      title.replace(title.length() - 8, 8, "...");
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}

void MarkdownReaderActivity::saveProgress() const {
  if (!markdown || !hasActiveSection()) {
    return;
  }
  const int currentPage = getActiveCurrentPage();
  SpiBusMutex::Guard guard;
  HalFile f;
  if (Storage.openFileForWrite("MDR", markdown->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
    f.close();
  }
}

void MarkdownReaderActivity::loadProgress() {
  if (!markdown) {
    return;
  }

  SpiBusMutex::Guard guard;
  HalFile f;
  if (Storage.openFileForRead("MDR", markdown->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      savedPage = data[0] + (data[1] << 8);
      hasSavedPage = true;
    }
    f.close();
  }
}

bool MarkdownReaderActivity::hasActiveSection() const {
  if (useAstRenderer.load()) {
    return mdSection != nullptr;
  }
  return htmlSection != nullptr;
}

uint16_t MarkdownReaderActivity::getActivePageCount() const {
  if (useAstRenderer.load()) {
    return mdSection ? mdSection->pageCount : 0;
  }
  return htmlSection ? htmlSection->pageCount : 0;
}

int MarkdownReaderActivity::getActiveCurrentPage() const {
  if (useAstRenderer.load()) {
    return mdSection ? mdSection->currentPage : 0;
  }
  return htmlSection ? htmlSection->currentPage : 0;
}

void MarkdownReaderActivity::setActiveCurrentPage(int page) {
  if (useAstRenderer.load()) {
    if (mdSection) {
      mdSection->currentPage = page;
    }
    return;
  }
  if (htmlSection) {
    htmlSection->currentPage = page;
  }
}

std::unique_ptr<Page> MarkdownReaderActivity::loadActivePage() {
  if (useAstRenderer.load()) {
    return mdSection ? mdSection->loadPageFromSectionFile() : nullptr;
  }
  return htmlSection ? htmlSection->loadPageFromSectionFile() : nullptr;
}

void MarkdownReaderActivity::jumpToNextHeading() {
  if (!astReady.load() || !markdown || !hasActiveSection()) {
    return;
  }

  const auto* nav = markdown->getNavigation();
  if (!nav) {
    return;
  }

  auto nextPage = nav->findNextHeading(static_cast<size_t>(getActiveCurrentPage()));
  if (nextPage.has_value() && nextPage.value() < static_cast<size_t>(getActivePageCount())) {
    setActiveCurrentPage(static_cast<int>(nextPage.value()));
    requestUpdate();
  }
}

void MarkdownReaderActivity::jumpToPrevHeading() {
  if (!astReady.load() || !markdown || !hasActiveSection()) {
    return;
  }

  const auto* nav = markdown->getNavigation();
  if (!nav) {
    return;
  }

  auto prevPage = nav->findPrevHeading(static_cast<size_t>(getActiveCurrentPage()));
  if (prevPage.has_value()) {
    setActiveCurrentPage(static_cast<int>(prevPage.value()));
    requestUpdate();
  }
}

void MarkdownReaderActivity::showTableOfContents() {
  if (!astReady.load() || !markdown || !hasActiveSection()) {
    return;
  }

  auto* nav = markdown->getNavigation();
  if (!nav || nav->getToc().empty()) {
    return;
  }

  exitActivity();
  enterNewActivity(new TocActivity(
      renderer, mappedInput, nav->getToc(),
      [this] {
        exitActivity();
        requestUpdate();
      },
      [this, nav](size_t tocIndex) {
        auto page = nav->findHeadingPage(tocIndex);
        if (page.has_value() && page.value() < static_cast<size_t>(getActivePageCount())) {
          setActiveCurrentPage(static_cast<int>(page.value()));
        } else if (getActivePageCount() > 0) {
          setActiveCurrentPage(0);
        }
        exitActivity();
        requestUpdate();
      }));
}
