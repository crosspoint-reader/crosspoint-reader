#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Typesetter.h>
#include <Typesetter/Page.h>
#include <Typesetter/ParsedText.h>
#include <Typesetter/blocks/BlockStyle.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading

bool isWhitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < static_cast<int>(cache.pageCount) - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

bool TxtReaderActivity::buildSectionCache(uint16_t viewportWidth, uint16_t viewportHeight) {
  const int fontId = cachedFontId;
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;
  const bool hyphenationEnabled = SETTINGS.hyphenationEnabled;
  const bool focusReadingEnabled = SETTINGS.focusReadingEnabled;

  if (!cache.openForWrite()) {
    return false;
  }
  cache.writeHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                    hyphenationEnabled, false, 0, focusReadingEnabled);

  std::vector<PageLutEntry> lut;

  const auto userAlign = static_cast<CssTextAlign>(paragraphAlignment);
  const auto defaultAlign = (userAlign == CssTextAlign::None) ? CssTextAlign::Left : userAlign;

  Typesetter typesetter(renderer, fontId, lineCompression, extraParagraphSpacing, viewportWidth, viewportHeight,
                        [this, &lut](std::unique_ptr<Page> page, uint16_t paragraphIndex, uint16_t listItemIndex) {
                          lut.push_back({cache.writePage(std::move(page)), paragraphIndex, listItemIndex});
                        });

  const size_t fileSize = txt->getFileSize();
  auto* buffer = static_cast<uint8_t*>(malloc(CHUNK_SIZE + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate read buffer");
    cache.closeAndRemove();
    return false;
  }

  size_t offset = 0;
  auto currentBlock =
      std::unique_ptr<ParsedText>(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled));
  BlockStyle blockStyle;
  blockStyle.alignment = defaultAlign;
  blockStyle.textAlignDefined = true;
  currentBlock->setBlockStyle(blockStyle);

  int paragraphCount = 0;

  while (offset < fileSize) {
    const size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
    if (!txt->readContent(buffer, offset, chunkSize)) {
      break;
    }
    buffer[chunkSize] = '\0';

    size_t pos = 0;
    while (pos < chunkSize) {
      if (buffer[pos] == '\n') {
        pos++;

        if (!currentBlock->isEmpty()) {
          typesetter.submitParagraph(std::move(currentBlock));
          paragraphCount++;
        }

        currentBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled));
        currentBlock->setBlockStyle(blockStyle);
        continue;
      }

      if (buffer[pos] == '\r') {
        pos++;
        continue;
      }

      size_t wordStart = pos;
      while (pos < chunkSize && !isWhitespace(static_cast<char>(buffer[pos]))) {
        pos++;
      }

      if (pos > wordStart) {
        // Avoid splitting a word across chunk boundaries
        bool atChunkBoundary = (pos == chunkSize) && (offset + pos < fileSize);
        if (atChunkBoundary) {
          offset += wordStart;
          goto next_chunk;
        }
        std::string token(reinterpret_cast<char*>(buffer + wordStart), pos - wordStart);
        currentBlock->addWord(std::move(token), EpdFontFamily::REGULAR);
      }

      while (pos < chunkSize && (buffer[pos] == ' ' || buffer[pos] == '\t')) {
        pos++;
      }
    }

    offset += chunkSize;
  next_chunk:

    // Yield to other tasks periodically
    if ((offset & 0xFFFF) == 0) {
      vTaskDelay(1);
    }
  }

  free(buffer);

  if (!currentBlock->isEmpty()) {
    typesetter.submitParagraph(std::move(currentBlock));
  }
  typesetter.finish();

  std::vector<std::pair<std::string, uint16_t>> emptyAnchors;
  if (!cache.finalize(lut, emptyAnchors)) {
    LOG_ERR("TRS", "Failed to finalize section cache");
    return false;
  }

  LOG_DBG("TRS", "Built section cache: %d pages from %d paragraphs", cache.pageCount, paragraphCount);
  return true;
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  const uint8_t screenMargin = SETTINGS.screenMargin;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += screenMargin;
  cachedOrientedMarginLeft += screenMargin;
  cachedOrientedMarginRight += screenMargin;
  cachedOrientedMarginBottom +=
      std::max(screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const uint16_t viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;

  if (!cache.loadHeader(cachedFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, false,
                        0, SETTINGS.focusReadingEnabled)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));

    if (!buildSectionCache(viewportWidth, viewportHeight)) {
      LOG_ERR("TRS", "Failed to build section cache");
      initialized = true;
      return;
    }
  }

  loadProgress();
  initialized = true;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (cache.pageCount == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= static_cast<int>(cache.pageCount)) currentPage = cache.pageCount - 1;

  // Load current page from section cache
  auto page = cache.loadPage(currentPage);
  if (!page) {
    LOG_ERR("TRS", "Failed to load page %d", currentPage);
    return;
  }

  renderer.clearScreen();
  renderContents(std::move(page));

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderContents(std::unique_ptr<Page> page) {
  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop);
  scope.endScanAndPrewarm();

  // BW rendering
  page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop);
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(
        renderer, [&]() { page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = cache.pageCount > 0 ? (currentPage + 1) * 100.0f / cache.pageCount : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, cache.pageCount, title);
}

void TxtReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= static_cast<int>(cache.pageCount)) {
        currentPage = cache.pageCount - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, cache.pageCount);
    }
  }
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = cache.pageCount;
  info.progressPercent =
      cache.pageCount > 0 ? static_cast<int>((currentPage + 1) * 100.0f / cache.pageCount + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
