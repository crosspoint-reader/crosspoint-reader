#include "MdReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <functional>
#include <numeric>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "MdReaderTocSelectionActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;
constexpr size_t MAX_LINE_LENGTH = 64 * 1024;
constexpr uint32_t CACHE_MAGIC = 0x4D4B4449;  // "MKDI"
constexpr uint8_t CACHE_VERSION = 5;          // Bumped: grid-rendered pipe tables

static std::string flattenHeadingText(const MdParser::ParsedLine& parsed) {
  std::string result;
  for (const auto& span : parsed.spans) {
    result += span.text;
  }
  return result;
}
}  // namespace

uint32_t MdReaderActivity::TableContext::pack() const {
  uint32_t v = cols & 0xF;
  for (uint8_t c = 0; c < MAX_TABLE_COLS; c++) {
    v |= static_cast<uint32_t>(aligns[c] & 0x3) << (4 + 2 * c);
  }
  return v;
}

MdReaderActivity::TableContext MdReaderActivity::TableContext::unpack(const uint32_t v) {
  TableContext ctx;
  ctx.cols = static_cast<uint8_t>(v & 0xF);
  if (ctx.cols > MAX_TABLE_COLS && ctx.cols != COLS_FLATTENED) {
    ctx.cols = 0;  // corrupt cache entry: treat as "no open table"
  }
  for (uint8_t c = 0; c < MAX_TABLE_COLS; c++) {
    ctx.aligns[c] = static_cast<uint8_t>((v >> (4 + 2 * c)) & 0x3);
  }
  return ctx;
}

void MdReaderActivity::assignHeadingPageNumbers() {
  if (pageOffsets.empty()) {
    return;
  }
  for (auto& heading : headings) {
    const auto it = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), heading.offset);
    heading.pageIndex = static_cast<int>((it == pageOffsets.begin()) ? 0 : (it - pageOffsets.begin() - 1));
  }
}

int MdReaderActivity::getHeadingIndexForOffset(size_t offset) const {
  if (headings.empty()) {
    return -1;
  }
  int index = -1;
  for (int i = 0; i < static_cast<int>(headings.size()); i++) {
    if (headings[i].offset <= offset) {
      index = i;
    } else {
      break;
    }
  }
  return index;
}

void MdReaderActivity::jumpToHeading(bool next) {
  if (headings.empty() || pageOffsets.empty()) {
    return;
  }

  const size_t currentOffset = (currentPage >= 0 && currentPage < totalPages) ? pageOffsets[currentPage] : 0;
  int headingIndex = getHeadingIndexForOffset(currentOffset);

  if (headingIndex < 0) {
    headingIndex = next ? 0 : static_cast<int>(headings.size()) - 1;
  } else {
    headingIndex += next ? 1 : -1;
  }

  if (headingIndex < 0) {
    headingIndex = 0;
  } else if (headingIndex >= static_cast<int>(headings.size())) {
    headingIndex = static_cast<int>(headings.size()) - 1;
  }

  const size_t headingOffset = headings[headingIndex].offset;
  const auto it = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), headingOffset);
  if (it == pageOffsets.begin()) {
    currentPage = 0;
  } else {
    currentPage = static_cast<int>(it - pageOffsets.begin() - 1);
  }
  currentHeadingIndex = headingIndex;
  requestUpdate();
}

void MdReaderActivity::scanHeadings() {
  headings.clear();
  if (!txt) {
    return;
  }

  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    return;
  }

  std::string pending;
  size_t pendingOffset = 0;
  bool hasPending = false;
  bool inCodeBlock = false;

  pageBuffer.resize(CHUNK_SIZE + 1);
  size_t offset = 0;

  while (offset < fileSize) {
    const size_t toRead = std::min(CHUNK_SIZE, fileSize - offset);
    if (!txt->readContent(pageBuffer.data(), offset, toRead)) {
      return;
    }
    pageBuffer[toRead] = '\0';

    size_t pos = 0;
    while (pos < toRead) {
      size_t lineEnd = pos;
      while (lineEnd < toRead && pageBuffer[lineEnd] != '\n') {
        lineEnd++;
      }

      const bool hasNewline = (lineEnd < toRead && pageBuffer[lineEnd] == '\n');
      const bool fileHasMore = (offset + toRead < fileSize);
      const size_t rawLen = lineEnd - pos;
      const bool hasCR = (rawLen > 0 && pageBuffer[pos + rawLen - 1] == '\r');
      const size_t displayLen = hasCR ? rawLen - 1 : rawLen;
      const size_t lineStartOffset = hasPending ? pendingOffset : (offset + pos);

      std::string rawLine;
      if (hasPending) {
        rawLine = std::move(pending);
        pending.clear();
        hasPending = false;
      }
      rawLine.append(reinterpret_cast<char*>(pageBuffer.data() + pos), displayLen);

      if (!hasNewline && fileHasMore) {
        if (!hasPending) {
          pendingOffset = lineStartOffset;
        }
        pending = std::move(rawLine);
        hasPending = true;
        break;
      }

      if (MdParser::isCodeFence(rawLine)) {
        inCodeBlock = !inCodeBlock;
      }
      const MdParser::ParsedLine parsed = MdParser::parseLine(rawLine, inCodeBlock);
      if (parsed.blockType == MdParser::BlockType::Header1 || parsed.blockType == MdParser::BlockType::Header2 ||
          parsed.blockType == MdParser::BlockType::Header3) {
        int level = 1;
        if (parsed.blockType == MdParser::BlockType::Header2) {
          level = 2;
        } else if (parsed.blockType == MdParser::BlockType::Header3) {
          level = 3;
        }
        headings.push_back({lineStartOffset, level, flattenHeadingText(parsed)});
      }

      pos = hasNewline ? lineEnd + 1 : lineEnd;
    }

    offset += toRead;
  }

  if (hasPending) {
    if (MdParser::isCodeFence(pending)) {
      inCodeBlock = !inCodeBlock;
    }
    const MdParser::ParsedLine parsed = MdParser::parseLine(pending, inCodeBlock);
    if (parsed.blockType == MdParser::BlockType::Header1 || parsed.blockType == MdParser::BlockType::Header2 ||
        parsed.blockType == MdParser::BlockType::Header3) {
      int level = 1;
      if (parsed.blockType == MdParser::BlockType::Header2) {
        level = 2;
      } else if (parsed.blockType == MdParser::BlockType::Header3) {
        level = 3;
      }
      headings.push_back({pendingOffset, level, flattenHeadingText(parsed)});
    }
  }
}

void MdReaderActivity::onReaderExit() {
  pageCodeBlockState.clear();
  pageTableState.clear();
  currentPageLines.clear();
}

bool MdReaderActivity::onConfirmShortPress() {
  if (headings.empty()) {
    return false;
  }
  onPageChanged();
  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<MdReaderTocSelectionActivity>(renderer, mappedInput, headings, currentHeadingIndex),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          currentPage = std::get<PageResult>(result.data).page;
          onPageChanged();
          requestUpdate();
        }
      });
  return true;
}

void MdReaderActivity::onPageChanged() {
  currentHeadingIndex = pageOffsets.empty() ? -1 : getHeadingIndexForOffset(pageOffsets[currentPage]);
}

void MdReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  computeViewportLayout();

  pageBuffer.reserve(CHUNK_SIZE + 1);
  scanHeadings();

  if (!loadPageIndexCache()) {
    buildPageIndex();
    savePageIndexCache();
  }
  assignHeadingPageNumbers();

  loadProgress();

  initialized = true;
}

int MdReaderActivity::measureSpans(const std::vector<MdParser::Span>& spans) const {
  return std::accumulate(spans.begin(), spans.end(), 0, [this](int acc, const MdParser::Span& span) {
    return acc + (span.text.empty() ? 0 : renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style));
  });
}

bool MdReaderActivity::wrapSpans(const std::vector<MdParser::Span>& spans, const int availableWidth, const int maxLines,
                                 std::vector<std::vector<MdParser::Span>>& outLines,
                                 std::vector<int>& outWidths) const {
  const size_t startSize = outLines.size();

  std::vector<MdParser::Span> currentLine;
  int currentWidth = 0;
  bool fullyConsumed = true;

  auto emitLine = [&]() {
    outLines.push_back(std::move(currentLine));
    outWidths.push_back(currentWidth);
    currentLine = std::vector<MdParser::Span>();
    currentWidth = 0;
  };

  for (const auto& span : spans) {
    if (span.text.empty()) continue;

    const int spanWidth = renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style);

    if (currentWidth + spanWidth <= availableWidth) {
      currentLine.push_back(span);
      currentWidth += spanWidth;
      continue;
    }

    // Need to break within this span
    std::string remaining = span.text;
    const auto style = span.style;

    while (!remaining.empty()) {
      // Check line limit (using lines added, not total size)
      if (static_cast<int>(outLines.size() - startSize) >= maxLines) {
        fullyConsumed = false;
        goto done;
      }

      const int remWidth = renderer.getTextAdvanceX(cachedFontId, remaining.c_str(), style);

      if (currentWidth + remWidth <= availableWidth) {
        currentLine.push_back({remaining, style});
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
        if (currentLine.empty()) {
          breakPos = 1;
          while (breakPos < remaining.size() && (remaining[breakPos] & 0xC0) == 0x80) {
            breakPos++;
          }
        } else {
          emitLine();
          continue;
        }
      }

      currentLine.push_back({remaining.substr(0, breakPos), style});
      currentWidth += renderer.getTextAdvanceX(cachedFontId, currentLine.back().text.c_str(), style);
      emitLine();

      size_t skip = breakPos;
      if (skip < remaining.size() && remaining[skip] == ' ') {
        skip++;
      }
      remaining = remaining.substr(skip);
    }
  }

done:
  if (!currentLine.empty()) {
    emitLine();
  }
  return fullyConsumed;
}

bool MdReaderActivity::wordWrapParsedLine(const MdParser::ParsedLine& parsed, int indent,
                                          std::vector<RenderedLine>& outLines, int maxLines, bool isCodeBlock) {
  if (parsed.spans.empty()) {
    RenderedLine rl;
    rl.indent = indent;
    rl.isHR = (parsed.blockType == MdParser::BlockType::HorizontalRule);
    outLines.push_back(std::move(rl));
    return true;
  }

  const int availableWidth = viewportWidth - indent;
  if (availableWidth <= 0) return true;

  // Build a flat list of all spans, prepending the list prefix if present.
  std::vector<MdParser::Span> allSpans;
  allSpans.reserve(parsed.spans.size() + 1);
  if (!parsed.listPrefix.empty()) {
    allSpans.push_back({parsed.listPrefix, EpdFontFamily::REGULAR});
  }
  allSpans.insert(allSpans.end(), parsed.spans.begin(), parsed.spans.end());

  const int listPrefixIndent =
      !parsed.listPrefix.empty()
          ? renderer.getTextAdvanceX(cachedFontId, parsed.listPrefix.c_str(), EpdFontFamily::REGULAR)
          : 0;

  // Transient span-line holders; the spans are moved into the RenderedLines.
  std::vector<std::vector<MdParser::Span>> lines;
  std::vector<int> widths;
  const bool fullyConsumed = wrapSpans(allSpans, availableWidth, maxLines, lines, widths);

  if (lines.empty()) {
    // Spans with no text (an empty line inside a code block): still one blank line.
    RenderedLine rl;
    rl.indent = indent;
    rl.isCodeBlock = isCodeBlock;
    outLines.push_back(std::move(rl));
    return fullyConsumed;
  }

  outLines.reserve(outLines.size() + lines.size());
  for (size_t i = 0; i < lines.size(); i++) {
    RenderedLine rl;
    rl.spans = std::move(lines[i]);
    rl.indent = (i == 0) ? indent : indent + listPrefixIndent;
    rl.isCodeBlock = isCodeBlock;
    outLines.push_back(std::move(rl));
  }
  return fullyConsumed;
}

bool MdReaderActivity::layoutTableRow(const MdParser::ParsedLine& parsed, const TableContext& ctx, const bool isHeader,
                                      const bool drawTopRule, const int maxLines, std::vector<RenderedLine>& outLines) {
  if (maxLines <= 0) return false;
  const uint8_t cols = ctx.cols;
  const int colWidth = viewportWidth / cols;
  const int innerWidth = colWidth - 2 * TABLE_CELL_PAD_X;

  // Per-cell wrapped lines, transient for this one row (never the whole table).
  // Bounded: at most cols × maxLines span lines, and wrapSpans stops at the cap,
  // so a pathological cell never builds lines just to throw them away.
  struct CellLayout {
    std::vector<std::vector<MdParser::Span>> lines;
    std::vector<int> widths;
  };
  CellLayout cells[MAX_TABLE_COLS];  // 8 × 2 empty vectors: stack headers only

  int rowLines = 1;  // an all-empty row still occupies one visual line
  const size_t cellCount = std::min(parsed.tableCells.size(), static_cast<size_t>(cols));
  for (size_t c = 0; c < cellCount; c++) {
    // Copy, not move: on failure the caller still owns the row for the
    // flattened fallback. Cell strings are short; this is a page-build path.
    std::vector<MdParser::Span> cellSpans = parsed.tableCells[c];
    if (isHeader) {
      for (auto& s : cellSpans) {
        s.style = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(s.style) |
                                                    static_cast<uint8_t>(EpdFontFamily::BOLD));
      }
    }
    if (!wrapSpans(cellSpans, innerWidth, maxLines, cells[c].lines, cells[c].widths)) {
      return false;  // this cell alone is taller than the space the row may use
    }
    rowLines = std::max(rowLines, static_cast<int>(cells[c].lines.size()));
  }
  if (rowLines > maxLines) return false;

  outLines.reserve(outLines.size() + rowLines);
  for (int li = 0; li < rowLines; li++) {
    RenderedLine rl;
    rl.tableCols = cols;
    if (li == 0 && drawTopRule) rl.tableFlags |= TBL_TOP;
    if (li == rowLines - 1) {
      rl.tableFlags |= TBL_ROW_END;
      if (isHeader) rl.tableFlags |= TBL_HEADER;
    }

    size_t spanCount = 0;
    for (size_t c = 0; c < cellCount; c++) {
      if (li < static_cast<int>(cells[c].lines.size())) spanCount += cells[c].lines[li].size();
    }
    rl.spans.reserve(spanCount);
    rl.spanX.reserve(spanCount);

    for (size_t c = 0; c < cellCount; c++) {
      if (li >= static_cast<int>(cells[c].lines.size())) continue;
      auto& cellLine = cells[c].lines[li];
      const int lineWidth = cells[c].widths[li];
      int x = static_cast<int>(c) * colWidth + TABLE_CELL_PAD_X;
      const auto align = static_cast<MdParser::TableAlign>(ctx.aligns[c]);
      if (align == MdParser::TableAlign::Center) {
        x += std::max(0, (innerWidth - lineWidth) / 2);
      } else if (align == MdParser::TableAlign::Right) {
        x += std::max(0, innerWidth - lineWidth);
      }
      for (auto& s : cellLine) {
        const int w = renderer.getTextAdvanceX(cachedFontId, s.text.c_str(), s.style);
        rl.spans.push_back(std::move(s));
        rl.spanX.push_back(static_cast<int16_t>(x));
        x += w;
      }
    }
    outLines.push_back(std::move(rl));
  }
  return true;
}

bool MdReaderActivity::appendFlattenedTableRow(const MdParser::ParsedLine& parsed, const bool isHeader,
                                               const int maxLines, std::vector<RenderedLine>& outLines) {
  // Flatten cells into a single flowed line: "Cell1  │  Cell2  │  Cell3".
  MdParser::ParsedLine flatLine;
  flatLine.blockType = MdParser::BlockType::Paragraph;
  size_t spanCount = parsed.tableCells.size();  // one divider between each pair of cells
  for (const auto& cell : parsed.tableCells) spanCount += cell.size();
  flatLine.spans.reserve(spanCount);

  bool first = true;
  for (const auto& cell : parsed.tableCells) {
    if (!first) {
      flatLine.spans.push_back({"  \xe2\x94\x82  ", EpdFontFamily::REGULAR});  // " │ "
    }
    first = false;
    for (auto span : cell) {
      if (isHeader) {
        span.style = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(span.style) |
                                                       static_cast<uint8_t>(EpdFontFamily::BOLD));
      }
      flatLine.spans.push_back(std::move(span));
    }
  }

  const bool fullyConsumed = wordWrapParsedLine(flatLine, 0, outLines, maxLines);

  // After a header row, add a thin separator line (reuses the isHR rendering).
  if (fullyConsumed && isHeader && static_cast<int>(outLines.size()) < linesPerPage) {
    RenderedLine sep;
    sep.isHR = true;
    outLines.push_back(sep);
  }
  return fullyConsumed;
}

bool MdReaderActivity::loadPageAtOffset(size_t offset, bool startInCodeBlock, TableContext startTable,
                                        std::vector<RenderedLine>& outLines, size_t& nextOffset, bool& endInCodeBlock,
                                        TableContext& endTable) {
  outLines.clear();
  endInCodeBlock = startInCodeBlock;
  endTable = startTable;
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  size_t bufferSize = std::min(CHUNK_SIZE, fileSize - offset);
  pageBuffer.resize(bufferSize + 1);
  if (!txt->readContent(pageBuffer.data(), offset, bufferSize)) {
    return false;
  }
  pageBuffer[bufferSize] = '\0';

  bool inCodeBlock = startInCodeBlock;
  // Table left open by the previous page (or opened on this one). A page that
  // starts mid-table draws its first grid line with a fresh top rule, closing
  // the box the page break opened.
  TableContext tableCtx = startTable;
  bool tableTopPending = startTable.active();
  size_t pos = 0;

  while (pos < bufferSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line and extend the read buffer if we are at chunk boundary.
    size_t lineEnd = pos;
    while (lineEnd < bufferSize && pageBuffer[lineEnd] != '\n') {
      lineEnd++;
    }

    while (lineEnd == bufferSize && offset + bufferSize < fileSize && bufferSize < MAX_LINE_LENGTH) {
      size_t extra = std::min(CHUNK_SIZE, fileSize - offset - bufferSize);
      if (bufferSize + extra > MAX_LINE_LENGTH) {
        extra = MAX_LINE_LENGTH - bufferSize;
      }
      if (extra == 0) {
        break;
      }

      pageBuffer.resize(bufferSize + extra + 1);
      if (!txt->readContent(pageBuffer.data() + bufferSize, offset + bufferSize, extra)) {
        return false;
      }
      bufferSize += extra;
      pageBuffer[bufferSize] = '\0';

      while (lineEnd < bufferSize && pageBuffer[lineEnd] != '\n') {
        lineEnd++;
      }
    }

    const bool isAtBufferEnd = (lineEnd == bufferSize);
    const bool isEOF = (offset + bufferSize >= fileSize);
    const bool lineComplete = (lineEnd < bufferSize) || isEOF || (isAtBufferEnd && bufferSize >= MAX_LINE_LENGTH);
    if (!lineComplete && !outLines.empty()) {
      // Incomplete line at chunk boundary and we already have content — stop here
      break;
    }

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && pageBuffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    std::string rawLine(reinterpret_cast<char*>(pageBuffer.data() + pos), displayLen);

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

    // GFM pipe tables. Separator rows are invisible; their alignment is
    // captured when the table's first row peeks ahead at them.
    if (parsed.blockType == MdParser::BlockType::TableSeparator) {
      pos = lineEnd + 1;
      continue;
    }
    if (parsed.blockType == MdParser::BlockType::TableRow && parsed.tableCells.empty()) {
      // A pipe line with no content: keep the old behaviour, a blank flowed line.
      parsed.blockType = MdParser::BlockType::Paragraph;
    }
    if (parsed.blockType == MdParser::BlockType::TableRow) {
      const bool startingTable = !tableCtx.active();
      bool isHeader = false;
      if (startingTable) {
        // Promote the table's first row to header if immediately followed by a
        // separator row, which also carries the per-column alignment. Peeks at
        // the next line in the buffer, like the flattened renderer always did.
        size_t nextLineStart = lineEnd + 1;
        size_t nextLineEnd = nextLineStart;
        while (nextLineEnd < bufferSize && pageBuffer[nextLineEnd] != '\n') nextLineEnd++;
        std::string nextRaw(reinterpret_cast<char*>(pageBuffer.data() + nextLineStart), nextLineEnd - nextLineStart);
        if (!nextRaw.empty() && nextRaw.back() == '\r') nextRaw.pop_back();

        tableCtx = TableContext();
        const size_t cellCount = parsed.tableCells.size();
        const int gridCols = static_cast<int>(std::min<size_t>(cellCount, MAX_TABLE_COLS));
        const bool tooNarrow = viewportWidth / gridCols - 2 * TABLE_CELL_PAD_X < MIN_COL_INNER_WIDTH;
        if (cellCount > MAX_TABLE_COLS || tooNarrow) {
          // The grid cannot represent this table; flatten it until it ends.
          tableCtx.cols = TableContext::COLS_FLATTENED;
        } else {
          tableCtx.cols = static_cast<uint8_t>(gridCols);
        }
        if (MdParser::isTableSeparator(nextRaw)) {
          isHeader = true;
          const auto aligns = MdParser::parseTableAlignments(nextRaw);
          for (size_t c = 0; c < aligns.size() && c < MAX_TABLE_COLS; c++) {
            tableCtx.aligns[c] = static_cast<uint8_t>(aligns[c]);
          }
        }
        tableTopPending = true;
      }

      {
        // SD fonts load glyphs on demand: warm the row's text (and the
        // flattened divider, cheap to include) before any measuring.
        std::string rowText = "\xe2\x94\x82";
        for (const auto& cell : parsed.tableCells) {
          for (const auto& span : cell) rowText += span.text;
        }
        renderer.ensureFontReady(cachedFontId, rowText.c_str());
      }

      const size_t linesBefore = outLines.size();
      const int remainingLines = linesPerPage - static_cast<int>(outLines.size());
      bool fullyConsumed;
      if (tableCtx.flattened()) {
        fullyConsumed = appendFlattenedTableRow(parsed, isHeader, remainingLines, outLines);
      } else {
        fullyConsumed = layoutTableRow(parsed, tableCtx, isHeader, tableTopPending, remainingLines, outLines);
        if (fullyConsumed) {
          tableTopPending = false;
        } else if (linesBefore == 0) {
          // The row alone is taller than a page; a grid row cannot be split,
          // so this one row degrades to the flattened form (which truncates).
          // The rows after it resume the grid as a fresh fragment.
          fullyConsumed = appendFlattenedTableRow(parsed, isHeader, remainingLines, outLines);
          tableTopPending = true;
        }
      }

      if (!fullyConsumed) {
        if (linesBefore > 0) {
          // Page was partially filled — roll the whole row to the next page.
          outLines.resize(linesBefore);
          if (startingTable) {
            tableCtx = TableContext();  // the first row rolls over; the next page re-detects its header
          }
        } else {
          // Even the flattened form of a page-sized row truncates: advance past it.
          pos = lineComplete ? lineEnd + 1 : lineEnd;
        }
        break;
      }

      pos = lineEnd + 1;
      continue;
    }
    // Any other block type (a blank line included) closes an open table.
    tableCtx = TableContext();

    // Determine indent (base + nesting level)
    int indent = 0;
    switch (parsed.blockType) {
      case MdParser::BlockType::UnorderedList:
      case MdParser::BlockType::OrderedList:
        indent = LIST_INDENT + std::min((int)parsed.indentLevel, 2) * LIST_INDENT;
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
      {
        std::string lineText;
        if (!parsed.listPrefix.empty()) lineText += parsed.listPrefix;
        for (const auto& span : parsed.spans) lineText += span.text;
        if (!lineText.empty()) renderer.ensureFontReady(cachedFontId, lineText.c_str());
      }

      size_t linesBefore = outLines.size();
      int remainingLines = linesPerPage - static_cast<int>(outLines.size());
      bool fullyConsumed = wordWrapParsedLine(parsed, indent, outLines, remainingLines,
                                              parsed.blockType == MdParser::BlockType::CodeBlock);

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
  endTable = tableCtx;
  return !outLines.empty();
}

void MdReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageCodeBlockState.clear();
  pageTableState.clear();

  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    totalPages = 0;
    LOG_DBG("MDR", "Empty markdown file, no pages");
    return;
  }

  pageOffsets.push_back(0);
  pageCodeBlockState.push_back(0);
  pageTableState.push_back(0);

  size_t offset = 0;
  bool inCodeBlock = false;
  TableContext tableCtx;

  LOG_DBG("MDR", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));  // overlays the displayed frame (drawPopup resyncs the write buffer)

  while (offset < fileSize) {
    std::vector<RenderedLine> tempLines;
    size_t nextOffset = offset;
    bool nextCodeBlock = inCodeBlock;
    TableContext nextTable;

    if (!loadPageAtOffset(offset, inCodeBlock, tableCtx, tempLines, nextOffset, nextCodeBlock, nextTable)) {
      break;
    }

    if (nextOffset <= offset) {
      break;
    }

    offset = nextOffset;
    inCodeBlock = nextCodeBlock;
    tableCtx = nextTable;

    if (offset < fileSize) {
      pageOffsets.push_back(offset);
      pageCodeBlockState.push_back(inCodeBlock ? 1 : 0);
      pageTableState.push_back(tableCtx.pack());
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
  TableContext startTable = (currentPage < static_cast<int>(pageTableState.size()))
                                ? TableContext::unpack(pageTableState[currentPage])
                                : TableContext();
  size_t nextOffset;
  bool endCodeBlock;
  TableContext endTable;
  currentPageLines.clear();
  loadPageAtOffset(offset, startCodeBlock, startTable, currentPageLines, nextOffset, endCodeBlock, endTable);

  renderer.clearScreen();
  renderPage();

  saveProgress();
}

int MdReaderActivity::drawSpanAt(const int x, const int y, const MdParser::Span& span) const {
  if (span.text.empty()) return 0;
  const int spanWidth = renderer.getTextAdvanceX(cachedFontId, span.text.c_str(), span.style);
  renderer.drawText(cachedFontId, x, y, span.text.c_str(), true, span.style);
  if ((span.style & EpdFontFamily::UNDERLINE) != 0) {
    const int underlineY = y + renderer.getFontAscenderSize(cachedFontId) + 3;
    renderer.drawLine(x, underlineY, x + spanWidth, underlineY, 2, true);
  }
  if ((span.style & EpdFontFamily::STRIKETHROUGH) != 0) {
    // Arbitrary vertical offset of 4px from the font midline to avoid colliding with typical diacritics;
    // adjust as needed
    const int strikeY = y + renderer.getFontAscenderSize(cachedFontId) / 2 + 4;
    renderer.drawLine(x, strikeY, x + spanWidth, strikeY, 2, true);
  }
  return spanWidth;
}

void MdReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  std::function<void()> renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (line.isHR) {
        // Draw horizontal rule as a thin line, centered at 50% width (25%→75%) of the
        // available content area rather than edge-to-edge, matching the conventional reader default.
        int hrY = y + lineHeight / 2;
        const int contentWidth = viewportWidth - line.indent;
        const int hrX = cachedOrientedMarginLeft + line.indent + contentWidth / 4;
        renderer.drawLine(hrX, hrY, hrX + contentWidth / 2, hrY);
        y += lineHeight;
      } else if (line.tableCols > 0) {
        // Table grid line: column rules span the line box; the spans carry
        // their own x (alignment was baked in at layout time).
        const int cols = line.tableCols;
        const int colWidth = viewportWidth / cols;
        const int x0 = cachedOrientedMarginLeft;
        const int tableRight = x0 + colWidth * cols;
        for (int c = 0; c <= cols; c++) {
          const int cx = x0 + c * colWidth;
          renderer.drawLine(cx, y, cx, y + lineHeight - 1);
        }
        if (line.tableFlags & TBL_TOP) {
          renderer.drawLine(x0, y, tableRight, y);
        }
        if (line.tableFlags & TBL_ROW_END) {
          const int ry = y + lineHeight - 1;
          renderer.drawLine(x0, ry, tableRight, ry);
          if (line.tableFlags & TBL_HEADER) {
            renderer.drawLine(x0, ry - 1, tableRight, ry - 1);
          }
        }
        for (size_t i = 0; i < line.spans.size(); i++) {
          drawSpanAt(x0 + line.spanX[i], y, line.spans[i]);
        }
        y += lineHeight;
      } else {
        if (line.isCodeBlock) {
          const int barX = cachedOrientedMarginLeft + std::max(line.indent - 6, 0);
          renderer.drawLine(barX, y + 2, barX, y + lineHeight - 2);
        }
        if (!line.spans.empty()) {
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
            x += drawSpanAt(x, y, span);
          }
        }
        y += lineHeight;
      }
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
    // Same AA pass as EpubReaderActivity — see TxtReaderActivity::renderPage()
    // for why the legacy store/restore helper ghosted after swapBuffers().
    renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
    // Never aborts: the navigation-preempt gate is currently EPUB-only (see
    // EpubReaderActivity::aaPreemptedByNavigation), so this pass keeps its previous behaviour.
    renderer.renderGrayscalePlanesSequential([&](GfxRenderer::RenderMode) { renderLines(); }, [] { return false; });
  }
}

void MdReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);

  noteStatusBarRendered();
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
  pageTableState.clear();
  pageTableState.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t pageOffset;
    serialization::readPod(f, pageOffset);
    uint8_t codeState;
    serialization::readPod(f, codeState);
    uint32_t tableState;
    serialization::readPod(f, tableState);
    pageOffsets.push_back(pageOffset);
    pageCodeBlockState.push_back(codeState);
    pageTableState.push_back(tableState);
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
    serialization::writePod(f, i < pageTableState.size() ? pageTableState[i] : 0u);
  }

  LOG_DBG("MDR", "Saved page index cache: %d pages", totalPages);
}

void MdReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  auto clampPage = [this]() {
    if (totalPages == 0) {
      currentPage = 0;
      return;
    }
    if (currentPage < 0) currentPage = 0;
    if (currentPage >= totalPages) currentPage = totalPages - 1;
  };
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      if (currentPage < totalPages - 1) {
        currentPage++;
        onPageChanged();
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_BACK:
      if (currentPage > 0) {
        currentPage--;
        onPageChanged();
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_FORWARD_10:
      currentPage += 10;
      clampPage();
      onPageChanged();
      requestUpdate();
      break;
    case BA::BTN_PAGE_BACK_10:
      currentPage -= 10;
      clampPage();
      onPageChanged();
      requestUpdate();
      break;
    case BA::BTN_NEXT_SECTION:
      jumpToHeading(true);
      break;
    case BA::BTN_PREV_SECTION:
      jumpToHeading(false);
      break;
    case BA::BTN_OPEN_TOC:
      if (!headings.empty()) {
        onPageChanged();
        startActivityForResult(
            std::make_unique<MdReaderTocSelectionActivity>(renderer, mappedInput, headings, currentHeadingIndex),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                currentPage = std::get<PageResult>(result.data).page;
                onPageChanged();
                requestUpdate();
              }
            });
      }
      break;
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      finish();
      break;
    case BA::BTN_CYCLE_ORIENTATION: {
      const uint8_t nextOrientation =
          static_cast<uint8_t>((SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT);
      SETTINGS.orientation = nextOrientation;
      SETTINGS.saveToFile();
      {
        // This runs on the loop task, while the render task reads the renderer's orientation
        // and pageOffsets on every pass. Rebuilding the index here, unlocked, cleared and
        // refilled the vector under a render that was indexing it. The lock covers the two
        // writes; the rebuild itself is left to render(), which runs initializeReader() under
        // the lock it already holds whenever `initialized` is false -- so the slow part does
        // not hold up this task, and nothing reads a half-built index.
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        initialized = false;
      }
      requestUpdate();
      break;
    }
    default:
      break;
  }
}
