#include "MdReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <numeric>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;
constexpr uint32_t CACHE_MAGIC = 0x4D4B4449;  // "MKDI"
constexpr uint8_t CACHE_VERSION = 3;          // Bumped: nested list indent + task checkboxes
}  // namespace

void MdReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  requestUpdate();
}

void MdReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  pageCodeBlockState.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void MdReaderActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void MdReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("MDR", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  if (!loadPageIndexCache()) {
    buildPageIndex();
    savePageIndexCache();
  }

  loadProgress();

  initialized = true;
}

int MdReaderActivity::measureSpans(const std::vector<MdParser::Span>& spans) const {
  return std::accumulate(spans.begin(), spans.end(), 0, [this](int acc, const MdParser::Span& span) {
    return acc + (span.text.empty() ? 0 : renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style));
  });
}

bool MdReaderActivity::wordWrapParsedLine(const MdParser::ParsedLine& parsed, int indent,
                                          std::vector<RenderedLine>& outLines, int maxLines) {
  const size_t startSize = outLines.size();

  if (parsed.spans.empty()) {
    RenderedLine rl;
    rl.indent = indent;
    rl.isHR = (parsed.blockType == MdParser::BlockType::HorizontalRule);
    outLines.push_back(std::move(rl));
    return true;
  }

  const int availableWidth = viewportWidth - indent;
  if (availableWidth <= 0) return true;

  // Build a flat list of all spans, prepending the list prefix if present
  std::vector<MdParser::Span> allSpans;
  if (!parsed.listPrefix.empty()) {
    allSpans.push_back({parsed.listPrefix, EpdFontFamily::REGULAR});
  }
  allSpans.insert(allSpans.end(), parsed.spans.begin(), parsed.spans.end());

  // Check if everything fits on one line
  int totalWidth = measureSpans(allSpans);
  if (totalWidth <= availableWidth) {
    RenderedLine rl;
    rl.spans = std::move(allSpans);
    rl.indent = indent;
    outLines.push_back(std::move(rl));
    return true;
  }

  // Word-wrap across spans
  RenderedLine currentLine;
  currentLine.indent = indent;
  int currentWidth = 0;
  bool fullyConsumed = true;

  for (size_t si = 0; si < allSpans.size(); si++) {
    const auto& span = allSpans[si];
    if (span.text.empty()) continue;

    int spanWidth = renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style);

    if (currentWidth + spanWidth <= availableWidth) {
      currentLine.spans.push_back(span);
      currentWidth += spanWidth;
      continue;
    }

    // Need to break within this span
    std::string remaining = span.text;
    auto style = span.style;

    while (!remaining.empty()) {
      // Check line limit (using lines added, not total size)
      if (static_cast<int>(outLines.size() - startSize) >= maxLines) {
        fullyConsumed = false;
        goto done;
      }

      int remWidth = renderer.getTextAdvanceX(cachedFontId, remaining.c_str(), style);

      if (currentWidth + remWidth <= availableWidth) {
        currentLine.spans.push_back({remaining, style});
        currentWidth += remWidth;
        remaining.clear();
        break;
      }

      size_t breakPos = remaining.size();

      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, remaining.substr(0, breakPos).c_str(), style) >
                                 availableWidth - currentWidth) {
        size_t spacePos = remaining.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          breakPos--;
          while (breakPos > 0 && (remaining[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        if (currentLine.spans.empty()) {
          breakPos = 1;
          while (breakPos < remaining.size() && (remaining[breakPos] & 0xC0) == 0x80) {
            breakPos++;
          }
        } else {
          outLines.push_back(std::move(currentLine));
          currentLine = RenderedLine();
          currentLine.indent = indent;
          currentWidth = 0;
          continue;
        }
      }

      currentLine.spans.push_back({remaining.substr(0, breakPos), style});
      outLines.push_back(std::move(currentLine));
      currentLine = RenderedLine();
      currentLine.indent = indent;
      currentWidth = 0;

      size_t skip = breakPos;
      if (skip < remaining.size() && remaining[skip] == ' ') {
        skip++;
      }
      remaining = remaining.substr(skip);
    }
  }

done:
  if (!currentLine.spans.empty()) {
    outLines.push_back(std::move(currentLine));
  }
  return fullyConsumed;
}

bool MdReaderActivity::loadPageAtOffset(size_t offset, bool startInCodeBlock, std::vector<RenderedLine>& outLines,
                                        size_t& nextOffset, bool& endInCodeBlock) {
  outLines.clear();
  endInCodeBlock = startInCodeBlock;
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("MDR", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  bool inCodeBlock = startInCodeBlock;
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && !outLines.empty()) {
      // Incomplete line at chunk boundary and we already have content — stop here
      break;
    }

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    std::string rawLine(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Check for code fence toggle
    bool wasFence = false;
    if (MdParser::isCodeFence(rawLine)) {
      inCodeBlock = !inCodeBlock;
      wasFence = true;
    }

    // Parse the markdown line
    MdParser::ParsedLine parsed;
    if (wasFence) {
      // Fence lines produce no visible output
      parsed.blockType = MdParser::BlockType::CodeBlock;
    } else {
      parsed = MdParser::parseLine(rawLine, inCodeBlock);
    }

    // Determine indent (base + nesting level)
    int indent = 0;
    switch (parsed.blockType) {
      case MdParser::BlockType::UnorderedList:
      case MdParser::BlockType::OrderedList:
        indent = LIST_INDENT + parsed.indentLevel * LIST_INDENT;
        break;
      case MdParser::BlockType::Blockquote:
        indent = BLOCKQUOTE_INDENT;
        break;
      case MdParser::BlockType::CodeBlock:
        if (!wasFence) indent = CODE_INDENT;
        break;
      default:
        break;
    }

    // Word-wrap and add to output (skip fence lines)
    if (!wasFence) {
      size_t linesBefore = outLines.size();
      int remainingLines = linesPerPage - static_cast<int>(outLines.size());
      bool fullyConsumed = wordWrapParsedLine(parsed, indent, outLines, remainingLines);

      if (!fullyConsumed) {
        if (linesBefore > 0) {
          // Page was partially filled — rollback this line and save it for next page
          outLines.resize(linesBefore);
          // Don't advance pos — next page re-processes this source line
        } else {
          // First line on page is longer than a full page — accept truncation, advance past it
          pos = lineComplete ? lineEnd + 1 : lineEnd;
        }
        break;
      }
    }

    // Advance past the newline (only if source line was fully consumed)
    pos = lineEnd + 1;
  }

  // Ensure progress
  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }

  nextOffset = offset + pos;
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  endInCodeBlock = inCodeBlock;
  free(buffer);

  return !outLines.empty();
}

void MdReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageCodeBlockState.clear();
  pageOffsets.push_back(0);
  pageCodeBlockState.push_back(0);

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();
  bool inCodeBlock = false;

  LOG_DBG("MDR", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<RenderedLine> tempLines;
    size_t nextOffset = offset;
    bool nextCodeBlock = inCodeBlock;

    if (!loadPageAtOffset(offset, inCodeBlock, tempLines, nextOffset, nextCodeBlock)) {
      break;
    }

    if (nextOffset <= offset) {
      break;
    }

    offset = nextOffset;
    inCodeBlock = nextCodeBlock;

    if (offset < fileSize) {
      pageOffsets.push_back(offset);
      pageCodeBlockState.push_back(inCodeBlock ? 1 : 0);
    }

    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("MDR", "Built page index: %d pages", totalPages);
}

void MdReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page
  size_t offset = pageOffsets[currentPage];
  bool startCodeBlock =
      (currentPage < static_cast<int>(pageCodeBlockState.size())) ? pageCodeBlockState[currentPage] : false;
  size_t nextOffset;
  bool endCodeBlock;
  currentPageLines.clear();
  loadPageAtOffset(offset, startCodeBlock, currentPageLines, nextOffset, endCodeBlock);

  renderer.clearScreen();
  renderPage();

  saveProgress();
}

void MdReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (line.isHR) {
        // Draw horizontal rule as a thin line
        int hrY = y + lineHeight / 2;
        renderer.drawLine(cachedOrientedMarginLeft + line.indent, hrY, cachedOrientedMarginLeft + viewportWidth, hrY);
      } else if (!line.spans.empty()) {
        int x = cachedOrientedMarginLeft + line.indent;

        // Apply text alignment for non-indented lines
        if (line.indent == 0) {
          int contentWidth = viewportWidth;
          switch (cachedParagraphAlignment) {
            case CrossPointSettings::CENTER_ALIGN: {
              x = cachedOrientedMarginLeft + (contentWidth - measureSpans(line.spans)) / 2;
              break;
            }
            case CrossPointSettings::RIGHT_ALIGN: {
              x = cachedOrientedMarginLeft + contentWidth - measureSpans(line.spans);
              break;
            }
            default:
              break;
          }
        }

        // Render each span
        for (const auto& span : line.spans) {
          if (!span.text.empty()) {
            renderer.drawText(cachedFontId, x, y, span.text.c_str(), true, span.style);
            x += renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style);
          }
        }
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
}

void MdReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void MdReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MDR", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void MdReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("MDR", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("MDR", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool MdReaderActivity::loadPageIndexCache() {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("MDR", cachePath, f)) {
    LOG_DBG("MDR", "No page index cache found");
    return false;
  }

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("MDR", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("MDR", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("MDR", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("MDR", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("MDR", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("MDR", "Cache font ID mismatch, rebuilding");
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("MDR", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("MDR", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  pageOffsets.clear();
  // Sanity check: reject corrupt cache with absurd page count
  if (numPages == 0 || numPages > 100000) {
    LOG_DBG("MDR", "Cache page count out of range (%u), rebuilding", numPages);
    return false;
  }

  pageOffsets.reserve(numPages);
  pageCodeBlockState.clear();
  pageCodeBlockState.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t pageOffset;
    serialization::readPod(f, pageOffset);
    uint8_t codeState;
    serialization::readPod(f, codeState);
    pageOffsets.push_back(pageOffset);
    pageCodeBlockState.push_back(codeState);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("MDR", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void MdReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("MDR", cachePath, f)) {
    LOG_ERR("MDR", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  for (size_t i = 0; i < pageOffsets.size(); i++) {
    serialization::writePod(f, static_cast<uint32_t>(pageOffsets[i]));
    serialization::writePod(f, pageCodeBlockState[i]);
  }

  LOG_DBG("MDR", "Saved page index cache: %d pages", totalPages);
}