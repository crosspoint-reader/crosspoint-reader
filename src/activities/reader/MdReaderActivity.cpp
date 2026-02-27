#include "MdReaderActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 25;
constexpr int progressBarMarginTop = 1;
constexpr size_t CHUNK_SIZE = 8 * 1024;
constexpr int BULLET_INDENT = 20;

// Cache magic distinct from TxtReader ("TXTI") to avoid collisions
constexpr uint32_t CACHE_MAGIC = 0x4D445249;  // "MDRI"
constexpr uint8_t CACHE_VERSION = 1;
}  // namespace

// ── Markdown parsing helpers ──────────────────────────────────────────────────

static std::string trimWhitespace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
  return s.substr(start, end - start);
}

static bool isHorizontalRule(const std::string& trimmed) {
  if (trimmed.size() < 3) return false;
  const char c = trimmed[0];
  if (c != '-' && c != '*' && c != '_') return false;
  for (char ch : trimmed) {
    if (ch != c) return false;
  }
  return true;
}

// Strip inline Markdown markers, showing link text only (not the URL).
static std::string stripInlineMarkdown(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    // Image: ![alt](url) → [image: alt]
    if (i + 1 < s.size() && s[i] == '!' && s[i + 1] == '[') {
      size_t altEnd = s.find(']', i + 2);
      if (altEnd != std::string::npos && altEnd + 1 < s.size() && s[altEnd + 1] == '(') {
        size_t urlEnd = s.find(')', altEnd + 2);
        if (urlEnd != std::string::npos) {
          result += "[image: ";
          result += s.substr(i + 2, altEnd - (i + 2));
          result += ']';
          i = urlEnd + 1;
          continue;
        }
      }
      result += s[i++];
      continue;
    }
    // Link: [text](url) → text (link text only, no URL)
    if (s[i] == '[') {
      size_t textEnd = s.find(']', i + 1);
      if (textEnd != std::string::npos && textEnd + 1 < s.size() && s[textEnd + 1] == '(') {
        size_t urlEnd = s.find(')', textEnd + 2);
        if (urlEnd != std::string::npos) {
          result += s.substr(i + 1, textEnd - (i + 1));
          i = urlEnd + 1;
          continue;
        }
      }
      result += s[i++];
      continue;
    }
    // Bold+italic: ***text***
    if (i + 2 < s.size() && s[i] == '*' && s[i + 1] == '*' && s[i + 2] == '*') {
      size_t end = s.find("***", i + 3);
      if (end != std::string::npos) {
        result += s.substr(i + 3, end - (i + 3));
        i = end + 3;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Bold: **text**
    if (i + 1 < s.size() && s[i] == '*' && s[i + 1] == '*') {
      size_t end = s.find("**", i + 2);
      if (end != std::string::npos) {
        result += s.substr(i + 2, end - (i + 2));
        i = end + 2;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Italic: *text*
    if (s[i] == '*') {
      size_t end = s.find('*', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Bold alt: __text__
    if (i + 1 < s.size() && s[i] == '_' && s[i + 1] == '_') {
      size_t end = s.find("__", i + 2);
      if (end != std::string::npos) {
        result += s.substr(i + 2, end - (i + 2));
        i = end + 2;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Italic alt: _text_ — only when preceded by space or at line start
    if (s[i] == '_' && (i == 0 || s[i - 1] == ' ')) {
      size_t end = s.find('_', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Inline code: `text`
    if (s[i] == '`') {
      size_t end = s.find('`', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    result += s[i++];
  }
  return result;
}

// Parse a single raw line into an MdLine. Updates inCodeFence state.
// Pass stripInline=false during page index building to skip the string
// allocation overhead of stripInlineMarkdown (width slightly overestimated,
// but page breaks are conservative and content is not affected).
static MdLine parseMdLine(const std::string& raw, bool& inCodeFence, bool stripInline = true) {
  MdLine result;
  const std::string trimmed = trimWhitespace(raw);

  // Code fence toggle: line starts with ```
  if (trimmed.size() >= 3 && trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') {
    inCodeFence = !inCodeFence;
    return result;  // Empty line for the fence delimiter itself
  }

  // Inside code fence — verbatim, no Markdown parsing
  if (inCodeFence) {
    result.text = raw;
    return result;
  }

  // Horizontal rule: trimmed line is 3+ of same char (-, *, _)
  if (isHorizontalRule(trimmed)) {
    result.isHRule = true;
    return result;
  }

  // Heading: 1–6 leading '#' chars followed by a space
  if (!trimmed.empty() && trimmed[0] == '#') {
    size_t level = 0;
    while (level < trimmed.size() && trimmed[level] == '#') level++;
    if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
      result.style = EpdFontFamily::BOLD;
      const std::string body = trimWhitespace(trimmed.substr(level + 1));
      result.text = stripInline ? stripInlineMarkdown(body) : body;
      return result;
    }
  }

  // Bullet: "- ", "+ " (not "* " to avoid ambiguity with HRule before this check)
  if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '+') && trimmed[1] == ' ') {
    result.indentPixels = BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = "\xe2\x80\xa2 " + (stripInline ? stripInlineMarkdown(body) : body);  // UTF-8 •
    return result;
  }
  // Bullet: "* " (safe here since HRule "***" was already matched above)
  if (trimmed.size() >= 2 && trimmed[0] == '*' && trimmed[1] == ' ') {
    result.indentPixels = BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = "\xe2\x80\xa2 " + (stripInline ? stripInlineMarkdown(body) : body);
    return result;
  }

  // Numbered list: digits followed by ". "
  if (!trimmed.empty() && trimmed[0] >= '1' && trimmed[0] <= '9') {
    size_t k = 0;
    while (k < trimmed.size() && trimmed[k] >= '0' && trimmed[k] <= '9') k++;
    if (k < trimmed.size() && trimmed[k] == '.' && k + 1 < trimmed.size() && trimmed[k + 1] == ' ') {
      result.indentPixels = BULLET_INDENT;
      const std::string body = trimWhitespace(trimmed.substr(k + 2));
      result.text = trimmed.substr(0, k + 1) + " " + (stripInline ? stripInlineMarkdown(body) : body);
      return result;
    }
  }

  // Blockquote: "> "
  if (trimmed.size() >= 2 && trimmed[0] == '>' && trimmed[1] == ' ') {
    result.style = EpdFontFamily::ITALIC;
    result.indentPixels = BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = stripInline ? stripInlineMarkdown(body) : body;
    return result;
  }

  // Normal paragraph text
  result.text = stripInline ? stripInlineMarkdown(raw) : raw;
  return result;
}

// ── Activity lifecycle ────────────────────────────────────────────────────────

void MdReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (!txt) return;

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

  txt->setupCacheDir();

  const auto filePath = txt->getPath();
  const auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  requestUpdate();
}

void MdReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  pageOffsets.clear();
  pageCodeFences.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void MdReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }
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

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }
}

// ── Initialization ────────────────────────────────────────────────────────────

void MdReaderActivity::initializeReader() {
  if (initialized) return;

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += cachedScreenMargin;

  auto metrics = UITheme::getInstance().getMetrics();
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - cachedScreenMargin +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("MRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  if (!loadPageIndexCache()) {
    buildPageIndex();
    savePageIndexCache();
  }
  loadProgress();
  initialized = true;
}

// ── Page index building ───────────────────────────────────────────────────────

void MdReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageCodeFences.clear();
  pageOffsets.push_back(0);
  pageCodeFences.push_back(false);

  size_t offset = 0;
  bool inCodeFence = false;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("MRS", "Building page index for %zu bytes...", fileSize);
  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<MdLine> tempLines;
    size_t nextOffset = offset;
    if (!loadPageAtOffset(offset, tempLines, nextOffset, inCodeFence, false)) break;
    if (nextOffset <= offset) break;

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
      pageCodeFences.push_back(inCodeFence);
    }
    if (pageOffsets.size() % 20 == 0) vTaskDelay(1);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("MRS", "Built page index: %d pages", totalPages);
}

// ── Page loading ──────────────────────────────────────────────────────────────

bool MdReaderActivity::loadPageAtOffset(size_t offset, std::vector<MdLine>& outLines, size_t& nextOffset,
                                        bool& inCodeFence, bool stripInline) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();
  if (offset >= fileSize) return false;

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("MRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }
  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  size_t pos = 0;
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;

    const bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);
    if (!lineComplete && !outLines.empty()) break;  // Incomplete line at chunk edge — stop here

    const size_t lineContentLen = lineEnd - pos;
    const bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    const size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;
    std::string rawLine(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Save fence state so we can restore it if this raw line doesn't fit on the current page
    const bool fenceBeforeLine = inCodeFence;

    MdLine mdLine = parseMdLine(rawLine, inCodeFence, stripInline);

    // Build word-wrapped sub-lines from this raw line
    std::vector<MdLine> subLines;
    if (mdLine.isHRule || mdLine.text.empty()) {
      subLines.push_back(mdLine);
    } else {
      const int effectiveWidth = viewportWidth - mdLine.indentPixels;
      std::string remaining = mdLine.text;
      while (!remaining.empty()) {
        if (renderer.getTextWidth(cachedFontId, remaining.c_str(), mdLine.style) <= effectiveWidth) {
          MdLine wl = mdLine;
          wl.text = remaining;
          subLines.push_back(wl);
          remaining.clear();
        } else {
          // Find break point
          size_t breakPos = remaining.length();
          while (breakPos > 0 &&
                 renderer.getTextWidth(cachedFontId, remaining.substr(0, breakPos).c_str(), mdLine.style) >
                     effectiveWidth) {
            size_t spacePos = remaining.rfind(' ', breakPos - 1);
            if (spacePos != std::string::npos && spacePos > 0) {
              breakPos = spacePos;
            } else {
              breakPos--;
              while (breakPos > 0 && (remaining[breakPos] & 0xC0) == 0x80) breakPos--;
            }
          }
          if (breakPos == 0) breakPos = 1;
          MdLine wl = mdLine;
          wl.text = remaining.substr(0, breakPos);
          subLines.push_back(wl);
          size_t skipChars = breakPos;
          if (breakPos < remaining.length() && remaining[breakPos] == ' ') skipChars++;
          remaining = remaining.substr(skipChars);
        }
      }
    }

    const int spaceLeft = linesPerPage - static_cast<int>(outLines.size());
    if (outLines.empty() || static_cast<int>(subLines.size()) <= spaceLeft) {
      // All sub-lines fit (or page is empty — must make progress regardless)
      const int toAdd = std::min((int)subLines.size(), spaceLeft > 0 ? spaceLeft : linesPerPage);
      for (int k = 0; k < toAdd; k++) outLines.push_back(subLines[k]);
      pos = lineEnd + 1;  // Always advance past the full raw line
    } else {
      // Doesn't fit — stop before this raw line and restore fence state
      inCodeFence = fenceBeforeLine;
      break;
    }
  }

  if (pos == 0 && !outLines.empty()) pos = 1;  // Safety: ensure forward progress
  nextOffset = offset + pos;
  if (nextOffset > fileSize) nextOffset = fileSize;

  free(buffer);
  return !outLines.empty();
}

// ── Rendering ─────────────────────────────────────────────────────────────────

void MdReaderActivity::render(Activity::RenderLock&&) {
  if (!txt) return;
  if (!initialized) initializeReader();

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  const size_t offset = pageOffsets[currentPage];
  bool inCodeFence = (currentPage < static_cast<int>(pageCodeFences.size())) ? pageCodeFences[currentPage] : false;
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset, inCodeFence);

  renderer.clearScreen();
  renderPage();
  renderer.clearFontCache();
  saveProgress();
}

void MdReaderActivity::renderPage() {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += statusBarMargin;

  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  auto renderLines = [&]() {
    int y = orientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (line.isHRule) {
        renderer.fillRect(orientedMarginLeft, y + lineHeight / 2, contentWidth, 1, true);
      } else if (!line.text.empty()) {
        int x = orientedMarginLeft + line.indentPixels;
        // Apply user alignment only for non-indented lines (headings/bullets always left-align)
        if (line.indentPixels == 0) {
          switch (cachedParagraphAlignment) {
            case CrossPointSettings::CENTER_ALIGN: {
              const int w = renderer.getTextWidth(cachedFontId, line.text.c_str(), line.style);
              x = orientedMarginLeft + (contentWidth - w) / 2;
              break;
            }
            case CrossPointSettings::RIGHT_ALIGN: {
              const int w = renderer.getTextWidth(cachedFontId, line.text.c_str(), line.style);
              x = orientedMarginLeft + contentWidth - w;
              break;
            }
            default:
              break;
          }
        }
        renderer.drawText(cachedFontId, x, y, line.text.c_str(), true, line.style);
      }
      y += lineHeight;
    }
  };

  // First pass: BW rendering + display
  renderLines();
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Optional grayscale anti-aliasing pass
  if (SETTINGS.textAntiAliasing) {
    renderer.storeBwBuffer();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderLines();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderLines();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
  }
}

void MdReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                       const int orientedMarginLeft) const {
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  auto metrics = UITheme::getInstance().getMetrics();
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d %.0f%%", currentPage + 1, totalPages, progress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", progress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage + 1, totalPages);
    }
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }
  if (showChapterProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showBattery) {
    GUI.drawBatteryLeft(renderer, Rect{orientedMarginLeft, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
  }

  if (showTitle) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;
    std::string title = txt->getTitle();
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTextWidth) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTextWidth);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }
    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}

// ── Progress persistence ──────────────────────────────────────────────────────

void MdReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4] = {static_cast<uint8_t>(currentPage & 0xFF), static_cast<uint8_t>((currentPage >> 8) & 0xFF), 0,
                       0};
    f.write(data, 4);
    f.close();
  }
}

void MdReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("MRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0) currentPage = 0;
      LOG_DBG("MRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}

// ── Page index cache ──────────────────────────────────────────────────────────
// Format: magic(4) | version(1) | fileSize(4) | viewportWidth(4) | linesPerPage(4)
//         fontId(4) | screenMargin(4) | paragraphAlignment(1)
//         numPages(4) | N × (offset(4) + codeFenceState(1))

bool MdReaderActivity::loadPageIndexCache() {
  std::string cachePath = txt->getCachePath() + "/md_index.bin";
  FsFile f;
  if (!Storage.openFileForRead("MRS", cachePath, f)) {
    LOG_DBG("MRS", "No page index cache found");
    return false;
  }

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    f.close();
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    f.close();
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    f.close();
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    f.close();
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    f.close();
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    f.close();
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    f.close();
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  pageOffsets.clear();
  pageCodeFences.clear();
  pageOffsets.reserve(numPages);
  pageCodeFences.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t off;
    serialization::readPod(f, off);
    pageOffsets.push_back(off);
    uint8_t fence;
    serialization::readPod(f, fence);
    pageCodeFences.push_back(fence != 0);
  }

  f.close();
  totalPages = pageOffsets.size();
  LOG_DBG("MRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void MdReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/md_index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("MRS", cachePath, f)) {
    LOG_ERR("MRS", "Failed to save page index cache");
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
    const uint8_t fence = (i < pageCodeFences.size() && pageCodeFences[i]) ? 1 : 0;
    serialization::writePod(f, fence);
  }

  f.close();
  LOG_DBG("MRS", "Saved page index cache: %d pages", totalPages);
}
