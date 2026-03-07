#include "MarkdownReaderActivity.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MarkdownToHtml.h>
#include <Serialization.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
}

void MarkdownReaderActivity::onEnter() {
  Activity::onEnter();

  if (!md) {
    return;
  }

  // Configure screen orientation
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

  md->setupCacheDir();

  // Save as last opened file and add to recent books
  auto filePath = md->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  requestUpdate();
}

void MarkdownReaderActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  section.reset();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  md.reset();
}

void MarkdownReaderActivity::loop() {
  // Long press BACK goes to file browser
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(md ? md->getPath() : "");
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

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (!section) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    section->currentPage = currentPage;
    requestUpdate();
  } else if (nextTriggered && currentPage < section->pageCount - 1) {
    currentPage++;
    section->currentPage = currentPage;
    requestUpdate();
  }
}

void MarkdownReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  const std::string sectionFilePath = md->getCachePath() + "/section.bin";
  section = std::unique_ptr<Section>(new Section(sectionFilePath, renderer));

  // Try loading cached section
  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                viewportHeight, SETTINGS.hyphenationEnabled, false, 0)) {
    LOG_DBG("MDR", "No cache, converting markdown...");

    const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

    // Convert MD → HTML temp file
    const std::string tmpHtmlPath = md->getCachePath() + "/tmp.html";
    if (!MarkdownToHtml::convert(md->getPath(), tmpHtmlPath)) {
      LOG_ERR("MDR", "Markdown conversion failed");
      section.reset();
      initialized = true;
      return;
    }

    // Parse HTML → section file using the EPUB pipeline
    uint16_t outPageCount = 0;
    if (!Section::createFromHtmlFile(tmpHtmlPath, sectionFilePath, renderer, SETTINGS.getReaderFontId(),
                                     SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                     SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                     SETTINGS.hyphenationEnabled, popupFn, outPageCount)) {
      LOG_ERR("MDR", "Section creation failed");
      Storage.remove(tmpHtmlPath.c_str());
      section.reset();
      initialized = true;
      return;
    }

    Storage.remove(tmpHtmlPath.c_str());
    section->pageCount = outPageCount;
    LOG_DBG("MDR", "Created section with %d pages", outPageCount);
  } else {
    LOG_DBG("MDR", "Loaded cached section: %d pages", section->pageCount);
  }

  loadProgress();
  section->currentPage = currentPage;
  initialized = true;
}

void MarkdownReaderActivity::render(RenderLock&&) {
  if (!md) {
    return;
  }

  if (!initialized) {
    initializeReader();
  }

  if (!section || section->pageCount == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= section->pageCount) currentPage = section->pageCount - 1;
  section->currentPage = currentPage;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  renderer.clearScreen();

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_ERR("MDR", "Failed to load page %d", currentPage);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  renderContents(std::move(page), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  renderer.clearFontCache();
  saveProgress();
}

void MarkdownReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                            const int orientedMarginRight, const int orientedMarginBottom,
                                            const int orientedMarginLeft) {
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Grayscale anti-aliased rendering
  if (SETTINGS.textAntiAliasing) {
    renderer.storeBwBuffer();

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
    renderer.restoreBwBuffer();
  }
}

void MarkdownReaderActivity::renderStatusBar() const {
  const float progress = section->pageCount > 0 ? (currentPage + 1) * 100.0f / section->pageCount : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = md->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, section->pageCount, title);
}

void MarkdownReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MDR", md->getCachePath() + "/progress.bin", f)) {
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
  FsFile f;
  if (Storage.openFileForRead("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (section && currentPage >= section->pageCount) {
        currentPage = section->pageCount - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("MDR", "Loaded progress: page %d", currentPage);
    }
    f.close();
  }
}
