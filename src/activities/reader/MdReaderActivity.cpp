#include "MdReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ─────────────────────────────────────────────────────────────────────────────
// Cache format constants
// ─────────────────────────────────────────────────────────────────────────────
namespace {
constexpr size_t   CHUNK_SIZE    = 8 * 1024;
constexpr uint32_t CACHE_MAGIC   = 0x4D445249;  // "MDRI"
constexpr uint8_t  CACHE_VERSION = 2;           // bumped: DisplayLine now uses runs

constexpr int HEADING_VSPACE = 4;   // extra px above H1/H2/H3
constexpr int LIST_VSPACE    = 1;   // extra px above first list-item line

// Maps the inline style bitmask (BOLD / ITALIC / CODE flags) to the
// EpdFontFamily::Style parameter expected by GfxRenderer draw calls.
// CODE is rendered in the mono font but with REGULAR weight.
EpdFontFamily::Style resolveStyle(uint8_t style) {
  const bool bold   = style & 0x01;  // MdReaderActivity::BOLD
  const bool italic = style & 0x02;  // MdReaderActivity::ITALIC
  if (bold && italic) return EpdFontFamily::BOLD_ITALIC;
  if (bold)           return EpdFontFamily::BOLD;
  if (italic)         return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Activity lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void MdReaderActivity::onEnter() {
  Activity::onEnter();
  if (!md) return;

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  mappedInput.setTouchOrientation(SETTINGS.orientation);
  md->setupCacheDir();

  auto filePath = md->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  requestUpdate();
}

void MdReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  mappedInput.setTouchOrientation(CrossPointSettings::PORTRAIT);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  md.reset();
}

void MdReaderActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(md ? md->getPath() : "");
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) return;

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────────────────────────────────────

void MdReaderActivity::initializeReader() {
  if (initialized) return;

  cachedBodyFontId = SETTINGS.getReaderFontId();
  cachedMonoFontId = UI_12_FONT_ID;
//  FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, OPENDYSLEXIC = 2, FONT_FAMILY_COUNT };
  switch (SETTINGS.fontFamily){
    case 0: // BOOKERLY
      cachedH1FontId         = BOOKERLY_18_FONT_ID;
      cachedH2FontId         = BOOKERLY_16_FONT_ID;
      cachedH3FontId         = BOOKERLY_14_FONT_ID;
      break;
    case 1: // NOTOSANS
    default:
      cachedH1FontId         = NOTOSANS_18_FONT_ID;
      cachedH2FontId         = NOTOSANS_16_FONT_ID;
      cachedH3FontId         = NOTOSANS_14_FONT_ID;
      break;
    case 2: // OPENDYSLEXIC
      cachedH1FontId         = OPENDYSLEXIC_14_FONT_ID;
      cachedH2FontId         = OPENDYSLEXIC_12_FONT_ID;
      cachedH3FontId         = OPENDYSLEXIC_10_FONT_ID;
      break;
  }

  cachedScreenMargin       = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  renderer.getOrientedViewableTRBL(
      &cachedOrientedMarginTop, &cachedOrientedMarginRight,
      &cachedOrientedMarginBottom, &cachedOrientedMarginLeft);
  cachedOrientedMarginTop    += cachedScreenMargin;
  cachedOrientedMarginLeft   += cachedScreenMargin;
  cachedOrientedMarginRight  += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin,
               static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;

  // Use H1 (tallest font) for linesPerPage so heading-heavy pages never overflow
  const int maxLineHeight = renderer.getLineHeight(cachedH1FontId);
  linesPerPage = viewportHeight / maxLineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("MDR", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  if (!loadPageIndexCache()) {
    buildPageIndex();
    savePageIndexCache();
  }

  loadProgress();
  initialized = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inline run parsing
//
// Walks the string and emits InlineRun segments, each with its own style flags.
// Handles: ***bold-italic***, **bold**, *italic*, __bold__, _italic_, `code`,
// and [link text](url).  Runs are emitted with the accumulated base style so
// that e.g. bold text inside a blockquote inherits no extra base flag.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<MdReaderActivity::InlineRun>
MdReaderActivity::parseInlineRuns(const std::string& s, uint8_t baseStyle) {
  std::vector<InlineRun> runs;
  std::string            cur;      // plain-text accumulator for current run
  size_t i = 0;

  auto flush = [&](uint8_t style) {
    if (!cur.empty()) {
      runs.push_back({cur, style});
      cur.clear();
    }
  };

  while (i < s.size()) {
    // ── Bold-italic: ***…*** or ___…___ ───────────────────────────────────
    if (i + 2 < s.size() &&
        ((s[i] == '*' && s[i+1] == '*' && s[i+2] == '*') ||
         (s[i] == '_' && s[i+1] == '_' && s[i+2] == '_'))) {
      char delim = s[i];
      size_t end = s.find(std::string(3, delim), i + 3);
      if (end != std::string::npos) {
        flush(baseStyle);
        runs.push_back({' '+s.substr(i + 3, end - (i + 3)),
                        static_cast<uint8_t>(baseStyle | BOLD | ITALIC)});
        i = end + 3;
        continue;
      }
    }

    // ── Bold: **…** or __…__ ─────────────────────────────────────────────
    if (i + 1 < s.size() &&
        ((s[i] == '*' && s[i+1] == '*') ||
         (s[i] == '_' && s[i+1] == '_'))) {
      char delim = s[i];
      size_t end = s.find(std::string(2, delim), i + 2);
      if (end != std::string::npos) {
        flush(baseStyle);
      //  s[i+1]=' ';
        runs.push_back({' '+s.substr(i + 2, end - (i + 2)),
                        static_cast<uint8_t>(baseStyle | BOLD)});
                        // was         runs.push_back({s.substr(i + 2, end - (i + 2)),
//                                                static_cast<uint8_t>(baseStyle | BOLD)});
        i = end + 2;
        continue;
      }
    }

    // ── Italic: *…* or _…_ ───────────────────────────────────────────────
    if ((s[i] == '*' || s[i] == '_') && (i == 0 || s[i-1] != s[i])) {
      char delim = s[i];
      size_t end = s.find(delim, i + 1);
      if (end != std::string::npos && (end + 1 >= s.size() || s[end+1] != delim)) {
        flush(baseStyle);
        runs.push_back({' '+s.substr(i + 1, end - (i + 1)),
                        static_cast<uint8_t>(baseStyle | ITALIC)});
        i = end + 1;
        continue;
      }
    }

    // ── Inline code: `…` ─────────────────────────────────────────────────
    if (s[i] == '`') {
      size_t end = s.find('`', i + 1);
      if (end != std::string::npos) {
        flush(baseStyle);
        runs.push_back({' '+s.substr(i + 1, end - (i + 1)),
                        static_cast<uint8_t>(baseStyle | CODE)});
        i = end + 1;
        continue;
      }
    }

    // ── Link: [text](url) → keep only "text" ─────────────────────────────
    if (s[i] == '[') {
      size_t closeBracket = s.find(']', i + 1);
      if (closeBracket != std::string::npos &&
          closeBracket + 1 < s.size() && s[closeBracket + 1] == '(') {
        size_t closeParen = s.find(')', closeBracket + 2);
        if (closeParen != std::string::npos) {
          flush(baseStyle);
          cur = s.substr(i + 1, closeBracket - (i + 1));
          flush(baseStyle);
          i = closeParen + 1;
          continue;
        }
      }
    }

    cur += s[i++];
  }

  flush(baseStyle);
  return runs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block-level parsing
// ─────────────────────────────────────────────────────────────────────────────

MdReaderActivity::ParsedBlock MdReaderActivity::parseBlockLine(const std::string& raw) {
  ParsedBlock out{BlockType::Text, {}};

  if (raw.empty()) {
    return out;  // blank line
  }

  // ── Horizontal rule: ---, ***, ___ ───────────────────────────────────────
  {
    size_t i = 0;
    while (i < raw.size() && raw[i] == ' ') i++;
    char c = (i < raw.size()) ? raw[i] : 0;
    if (c == '-' || c == '*' || c == '_') {
      size_t count = 0;
      size_t j = i;
      bool onlyRule = true;
      while (j < raw.size()) {
        if (raw[j] == c)       count++;
        else if (raw[j] != ' ') { onlyRule = false; break; }
        j++;
      }
      if (onlyRule && count >= 3) {
        out.block = BlockType::HRule;
        return out;
      }
    }
  }

  // ── ATX headings ─────────────────────────────────────────────────────────
  if (raw[0] == '#') {
    int level = 0;
    while (level < static_cast<int>(raw.size()) && raw[level] == '#') level++;
    if (level < static_cast<int>(raw.size()) && raw[level] == ' ') {
      std::string content = raw.substr(level + 1);
      size_t end = content.find_last_not_of(" #");
      if (end != std::string::npos) content = content.substr(0, end + 1);

      out.block = (level == 1) ? BlockType::H1
                : (level == 2) ? BlockType::H2
                               : BlockType::H3;
      // Headings are always bold; pass BOLD as the base style so every run
      // in the heading inherits it even if it has no extra inline markers.
      out.runs = parseInlineRuns(content, BOLD);
      return out;
    }
  }

  // ── Blockquote ───────────────────────────────────────────────────────────
  if (raw[0] == '>') {
    std::string content = (raw.size() > 1 && raw[1] == ' ') ? raw.substr(2) : raw.substr(1);
    out.block = BlockType::Blockquote;
    out.runs  = parseInlineRuns(content);
    return out;
  }

  // ── Unordered list item ──────────────────────────────────────────────────
  if (raw.size() >= 2 &&
      (raw[0] == '-' || raw[0] == '*' || raw[0] == '+') &&
      raw[1] == ' ') {
    out.block = BlockType::ListItem;
    out.runs  = parseInlineRuns(raw.substr(2));
    return out;
  }

  // ── Plain paragraph text ─────────────────────────────────────────────────
  out.block = BlockType::Text;
  out.runs  = parseInlineRuns(raw);
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Font and indent resolution
// ─────────────────────────────────────────────────────────────────────────────

int MdReaderActivity::resolveFontId(BlockType block, uint8_t style) const {
  switch (block) {
    case BlockType::H1: return cachedH1FontId;
    case BlockType::H2: return cachedH2FontId;
    case BlockType::H3: return cachedH3FontId;
    default:
      if (style & CODE) return cachedMonoFontId;
      return cachedBodyFontId;
  }
}

int MdReaderActivity::resolveIndent(BlockType block) const {
  switch (block) {
    case BlockType::Blockquote: return 16;
    case BlockType::ListItem:   return 20;
    default:                    return 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Run helpers
// ─────────────────────────────────────────────────────────────────────────────

size_t MdReaderActivity::runsLength(const std::vector<InlineRun>& runs) {
  size_t len = 0;
  for (const auto& r : runs) len += r.text.size();
  return len;
}

int MdReaderActivity::runsWidth(const std::vector<InlineRun>& runs, BlockType block) const {
  int total = 0;
  for (const auto& r : runs) {
    if (!r.text.empty()) {
      total += renderer.getTextWidth(resolveFontId(block, r.style), r.text.c_str(), resolveStyle(r.style));
    }
  }
  return total;
}

void MdReaderActivity::splitRunsAt(const std::vector<InlineRun>& runs, size_t charPos,
                                   std::vector<InlineRun>& head,
                                   std::vector<InlineRun>& tail) {
  head.clear();
  tail.clear();
  size_t remaining = charPos;

  for (size_t i = 0; i < runs.size(); i++) {
    const auto& r = runs[i];
    if (remaining == 0) {
      tail.insert(tail.end(), runs.begin() + i, runs.end());
      return;
    }
    if (r.text.size() <= remaining) {
      head.push_back(r);
      remaining -= r.text.size();
    } else {
      // Split within this run
      head.push_back({r.text.substr(0, remaining), r.style});
      std::string rest = r.text.substr(remaining);
      // Consume a leading space at the break point
      if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
      if (!rest.empty()) tail.push_back({rest, r.style});
      tail.insert(tail.end(), runs.begin() + i + 1, runs.end());
      return;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Page index build
// ─────────────────────────────────────────────────────────────────────────────

void MdReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);

  size_t offset = 0;
  const size_t fileSize = md->getFileSize();

  LOG_DBG("MDR", "Building page index for %zu bytes...", fileSize);
  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<DisplayLine> tempLines;
    size_t nextOffset = offset;
    if (!loadPageAtOffset(offset, tempLines, nextOffset)) break;
    if (nextOffset <= offset) break;

    offset = nextOffset;
    if (offset < fileSize) pageOffsets.push_back(offset);
    if (pageOffsets.size() % 20 == 0) vTaskDelay(1);
  }

  totalPages = static_cast<int>(pageOffsets.size());
  LOG_DBG("MDR", "Built page index: %d pages", totalPages);
}

// ─────────────────────────────────────────────────────────────────────────────
// Core page loader
// ─────────────────────────────────────────────────────────────────────────────

bool MdReaderActivity::loadPageAtOffset(size_t offset,
                                        std::vector<DisplayLine>& outLines,
                                        size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = md->getFileSize();
  if (offset >= fileSize) return false;

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("MDR", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!md->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  size_t pos = 0;
  BlockType prevBlock = BlockType::Text;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of raw source line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;

    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);
    if (!lineComplete && !outLines.empty()) break;

    size_t rawLen = lineEnd - pos;
    if (rawLen > 0 && buffer[pos + rawLen - 1] == '\r') rawLen--;
    std::string rawLine(reinterpret_cast<char*>(buffer + pos), rawLen);

    ParsedBlock parsed = parseBlockLine(rawLine);

    // ── HRule ───────────────────────────────────────────────────────────
    if (parsed.block == BlockType::HRule) {
      DisplayLine dl;
      dl.block          = BlockType::HRule;
      dl.isFirstInBlock = true;
      outLines.push_back(dl);
      prevBlock = BlockType::HRule;
      pos = lineEnd + 1;
      continue;
    }

    // ── Blank line ───────────────────────────────────────────────────────
    if (parsed.runs.empty()) {
      if (!outLines.empty()) {
        DisplayLine dl;
        dl.block = BlockType::Text;
        outLines.push_back(dl);
      }
      prevBlock = BlockType::Text;
      pos = lineEnd + 1;
      continue;
    }

    // ── Word-wrap inline runs ────────────────────────────────────────────
    const int  wrapWidth    = viewportWidth - resolveIndent(parsed.block);
    bool       isFirstWrap  = true;
    // Work on a mutable copy of the runs
    std::vector<InlineRun> remaining = parsed.runs;
    size_t consumedBytes = 0;  // bytes consumed from rawLine's plain text

    while (!remaining.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      if (runsWidth(remaining, parsed.block) <= wrapWidth) {
        DisplayLine dl;
        dl.runs           = remaining;
        dl.block          = parsed.block;
        dl.isFirstInBlock = isFirstWrap;
        outLines.push_back(dl);
        consumedBytes += runsLength(remaining);
        remaining.clear();
        isFirstWrap = false;
        break;
      }

      // Binary search for the largest plain-text prefix that fits.
      // We measure by rebuilding prefixes from runs, trying character counts.
      size_t totalChars = runsLength(remaining);
      size_t lo = 1, hi = totalChars, breakPos = 1;

      while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        std::vector<InlineRun> headTry, tailTry;
        splitRunsAt(remaining, mid, headTry, tailTry);
        if (runsWidth(headTry, parsed.block) <= wrapWidth) {
          breakPos = mid;
          lo = mid + 1;
        } else {
          if (mid == 0) break;
          hi = mid - 1;
        }
      }

      // Prefer breaking at a word boundary: scan left from breakPos for a space
      {
        // Rebuild the flat plain text to find a space
        std::string flat;
        for (const auto& r : remaining) flat += r.text;
        size_t spacePos = flat.rfind(' ', breakPos);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        }
        // Respect UTF-8 boundaries
        while (breakPos > 0 && (flat[breakPos] & 0xC0) == 0x80) breakPos--;
      }

      std::vector<InlineRun> head, tail;
      splitRunsAt(remaining, breakPos, head, tail);

      DisplayLine dl;
      dl.runs           = head;
      dl.block          = parsed.block;
      dl.isFirstInBlock = isFirstWrap;
      outLines.push_back(dl);
      isFirstWrap   = false;
      consumedBytes += breakPos + 1;  // +1 for consumed space
      remaining      = tail;
    }

    if (remaining.empty()) {
      pos = lineEnd + 1;
      prevBlock = parsed.block;
    } else {
      // Page full mid-line
      pos = pos + consumedBytes;
      break;
    }
  }

  if (pos == 0 && !outLines.empty()) pos = 1;
  nextOffset = offset + pos;
  if (nextOffset > fileSize) nextOffset = fileSize;

  free(buffer);
  return !outLines.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Render
// ─────────────────────────────────────────────────────────────────────────────

void MdReaderActivity::render(RenderLock&&) {
  if (!md) return;
  if (!initialized) initializeReader();

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();
  saveProgress();
}

void MdReaderActivity::renderPage() {
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    const int n = static_cast<int>(currentPageLines.size());

    for (int li = 0; li < n; li++) {
      const auto& line = currentPageLines[li];
      // The line height is driven by the tallest font among the runs.
      // For simplicity (and consistency with linesPerPage budget) we use
      // the block's heading font for heading lines, body otherwise.
      const int lineH  = renderer.getLineHeight(
          resolveFontId(line.block, line.runs.empty() ? NORMAL : line.runs[0].style));
      const int indent = resolveIndent(line.block);
      const int x0     = cachedOrientedMarginLeft + indent;

      // Blank lines adjacent to an HRule get halved so the rule sits tight.
      const bool nextIsHRule = (li + 1 < n) && (currentPageLines[li + 1].block == BlockType::HRule);
      const bool prevIsHRule = (li > 0)     && (currentPageLines[li - 1].block == BlockType::HRule);
      if (line.block == BlockType::Text && line.runs.empty() && (nextIsHRule || prevIsHRule)) {
        y += lineH / 2;
        continue;
      }

      // Extra space before the first line of certain blocks
      if (line.isFirstInBlock) {
        switch (line.block) {
          case BlockType::H1:
          case BlockType::H2:
          case BlockType::H3:
            y += HEADING_VSPACE;
            break;
          case BlockType::ListItem:
            y += LIST_VSPACE;
            break;
          default:
            break;
        }
      }

      // HRule — use half the normal line height so surrounding space is compact
      if (line.block == BlockType::HRule) {
        const int halfH = lineH / 2;
        const int ry    = y + halfH / 2;
        renderer.drawLine(cachedOrientedMarginLeft, ry,
                          cachedOrientedMarginLeft + viewportWidth, ry, 2, 3);
        y += halfH;
        continue;
      }

      // Blockquote accent bar
      if (line.block == BlockType::Blockquote) {
        renderer.drawLine(cachedOrientedMarginLeft + 4, y,
                          cachedOrientedMarginLeft + 4, y + lineH - 2, 3);
      }

      // List bullet
      if (line.block == BlockType::ListItem && line.isFirstInBlock) {
        renderer.drawRect(cachedOrientedMarginLeft + 6, y + lineH / 2 - 2, 5, 5, 2, true);
      }

      // Draw inline runs left-to-right, each in its own font
      if (!line.runs.empty()) {
        // Compute total line width for alignment
        int totalWidth = runsWidth(line.runs, line.block);
        int drawX = x0;

        if (line.block == BlockType::Text || line.block == BlockType::Blockquote) {
          switch (cachedParagraphAlignment) {
            case CrossPointSettings::CENTER_ALIGN:
              drawX = cachedOrientedMarginLeft + (viewportWidth - totalWidth) / 2;
              break;
            case CrossPointSettings::RIGHT_ALIGN:
              drawX = cachedOrientedMarginLeft + viewportWidth - totalWidth;
              break;
            default:
              break;
          }
        }

        for (const auto& run : line.runs) {
          if (run.text.empty()) continue;
          const int fontId = resolveFontId(line.block, run.style);
          const EpdFontFamily::Style fStyle = resolveStyle(run.style);
          renderer.drawText(fontId, drawX, y, run.text.c_str(), true, fStyle);
          drawX += renderer.getTextWidth(fontId, run.text.c_str(), fStyle);
        }
      }

      y += lineH;
    }
  };

  // Two-pass: scan for font-cache pre-warming, then real draw
  auto* fcm   = renderer.getFontCacheManager();
  auto  scope = fcm->createPrewarmScope();
  renderLines();
  scope.endScanAndPrewarm();

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
    title = md->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

// ─────────────────────────────────────────────────────────────────────────────
// Progress persistence
// ─────────────────────────────────────────────────────────────────────────────

void MdReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4] = {
        static_cast<uint8_t>(currentPage & 0xFF),
        static_cast<uint8_t>((currentPage >> 8) & 0xFF), 0, 0};
    f.write(data, 4);
    f.close();
  }
}

void MdReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0)           currentPage = 0;
      LOG_DBG("MDR", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Page-index cache
// ─────────────────────────────────────────────────────────────────────────────

bool MdReaderActivity::loadPageIndexCache() {
  std::string cachePath = md->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("MDR", cachePath, f)) {
    LOG_DBG("MDR", "No page index cache found");
    return false;
  }

  auto fail = [&](const char* reason) -> bool {
    LOG_DBG("MDR", "Cache invalid (%s), rebuilding", reason);
    f.close();
    return false;
  };

  uint32_t magic;   serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) return fail("magic mismatch");

  uint8_t version; serialization::readPod(f, version);
  if (version != CACHE_VERSION) return fail("version mismatch");

  uint32_t fileSize; serialization::readPod(f, fileSize);
  if (fileSize != md->getFileSize()) return fail("file size mismatch");

  int32_t vw; serialization::readPod(f, vw);
  if (vw != viewportWidth) return fail("viewport width mismatch");

  int32_t lpp; serialization::readPod(f, lpp);
  if (lpp != linesPerPage) return fail("lines per page mismatch");

  int32_t bFont; serialization::readPod(f, bFont);
  if (bFont != cachedBodyFontId) return fail("body font mismatch");

  int32_t h1; serialization::readPod(f, h1);
  if (h1 != cachedH1FontId) return fail("H1 font mismatch");

  int32_t h2; serialization::readPod(f, h2);
  if (h2 != cachedH2FontId) return fail("H2 font mismatch");

  int32_t h3; serialization::readPod(f, h3);
  if (h3 != cachedH3FontId) return fail("H3 font mismatch");

  int32_t margin; serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) return fail("screen margin mismatch");

  uint8_t align; serialization::readPod(f, align);
  if (align != cachedParagraphAlignment) return fail("paragraph alignment mismatch");

  uint32_t numPages; serialization::readPod(f, numPages);
  pageOffsets.clear();
  pageOffsets.reserve(numPages);
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t off; serialization::readPod(f, off);
    pageOffsets.push_back(off);
  }

  f.close();
  totalPages = static_cast<int>(pageOffsets.size());
  LOG_DBG("MDR", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void MdReaderActivity::savePageIndexCache() const {
  std::string cachePath = md->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("MDR", cachePath, f)) {
    LOG_ERR("MDR", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(md->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedBodyFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedH1FontId));
  serialization::writePod(f, static_cast<int32_t>(cachedH2FontId));
  serialization::writePod(f, static_cast<int32_t>(cachedH3FontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));
  for (size_t off : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(off));
  }

  f.close();
  LOG_DBG("MDR", "Saved page index cache: %d pages", totalPages);
}
