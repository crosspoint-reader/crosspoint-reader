# EPUB テーブル グリッドレイアウト表示 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** EPUBのHTMLテーブルを罫線付きグリッドレイアウトで表示する（現在の「Tab Row X, Cell Y:」平坦化を置換）

**Architecture:** テーブルデータをパース中にバッファし、`</table>` で列幅を算出。各行を新しい `TableRowBlock` / `PageTableRow` として行単位でページに追加し、罫線+セルテキストを描画する。小フォント（`SMALL_FONT_ID`）使用、セルテキストは1行・切り詰め方式。

**Tech Stack:** C++20, ESP32-C3 (PlatformIO), GfxRenderer (drawRect/drawLine/drawText)

---

## File Structure

| ファイル | 変更種別 | 責務 |
|---------|--------|------|
| `lib/Epub/Epub/blocks/Block.h` | 修正 | `TABLE_ROW_BLOCK` をBlockType enumに追加 |
| `lib/Epub/Epub/blocks/TableRowBlock.h` | **新規** | TableRowBlock クラス定義 |
| `lib/Epub/Epub/blocks/TableRowBlock.cpp` | **新規** | render / serialize / deserialize |
| `lib/Epub/Epub/Page.h` | 修正 | `PageTableRow` クラス + `TAG_PageTableRow` |
| `lib/Epub/Epub/Page.cpp` | 修正 | PageTableRow::render/serialize/deserialize + Page::deserialize分岐 |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` | 修正 | テーブルバッファ用データ構造 |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | 修正 | テーブル収集→グリッド生成ロジック |
| `lib/Epub/Epub/Section.cpp` | 修正 | SECTION_FILE_VERSION 20→21 |

---

### Task 1: BlockType に TABLE_ROW_BLOCK 追加

**Files:**
- Modify: `lib/Epub/Epub/blocks/Block.h:5`

- [ ] **Step 1: BlockType enum に TABLE_ROW_BLOCK を追加**

```cpp
// lib/Epub/Epub/blocks/Block.h — 行5を変更
typedef enum { TEXT_BLOCK, IMAGE_BLOCK, TABLE_ROW_BLOCK } BlockType;
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 3: コミット**

```bash
git add lib/Epub/Epub/blocks/Block.h
git commit -m "✨ BlockType に TABLE_ROW_BLOCK を追加"
```

---

### Task 2: TableRowBlock の作成

**Files:**
- Create: `lib/Epub/Epub/blocks/TableRowBlock.h`
- Create: `lib/Epub/Epub/blocks/TableRowBlock.cpp`

- [ ] **Step 1: TableRowBlock.h を作成**

```cpp
// lib/Epub/Epub/blocks/TableRowBlock.h
#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"

class GfxRenderer;

// Shared column layout computed once per table, referenced by all rows
struct TableColumnLayout {
  std::vector<uint16_t> colWidths;  // pixel width of each column
  int fontId = 0;                   // font used for table cell text
  int16_t rowHeight = 0;            // row height in pixels (lineHeight + padding)
  int16_t cellPadding = 2;          // horizontal padding inside each cell
};

// Represents one row of a table grid
class TableRowBlock final : public Block {
 public:
  TableRowBlock(std::vector<std::string> cellTexts, std::vector<bool> cellIsHeader,
                std::shared_ptr<TableColumnLayout> layout, bool isFirstRow, bool isLastRow);
  ~TableRowBlock() override = default;

  BlockType getType() override { return TABLE_ROW_BLOCK; }
  bool isEmpty() override { return cellTexts.empty(); }

  void render(GfxRenderer& renderer, int x, int y, int viewportWidth) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TableRowBlock> deserialize(FsFile& file);

  int16_t getRowHeight() const { return layout ? layout->rowHeight : 0; }

 private:
  std::vector<std::string> cellTexts;
  std::vector<bool> cellIsHeader;  // true = <th>, rendered bold
  std::shared_ptr<TableColumnLayout> layout;
  bool isFirstRow;  // draw top border
  bool isLastRow;   // draw bottom border + extra margin
};
```

- [ ] **Step 2: TableRowBlock.cpp を作成**

```cpp
// lib/Epub/Epub/blocks/TableRowBlock.cpp
#include "TableRowBlock.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

TableRowBlock::TableRowBlock(std::vector<std::string> cellTexts, std::vector<bool> cellIsHeader,
                             std::shared_ptr<TableColumnLayout> layout, const bool isFirstRow, const bool isLastRow)
    : cellTexts(std::move(cellTexts)),
      cellIsHeader(std::move(cellIsHeader)),
      layout(std::move(layout)),
      isFirstRow(isFirstRow),
      isLastRow(isLastRow) {}

void TableRowBlock::render(GfxRenderer& renderer, const int x, const int y, const int viewportWidth) const {
  if (!layout || layout->colWidths.empty()) return;

  const int fontId = layout->fontId;
  const int16_t rowH = layout->rowHeight;
  const int16_t pad = layout->cellPadding;
  const int numCols = static_cast<int>(layout->colWidths.size());

  // Calculate total table width and center it
  int totalTableWidth = 0;
  for (auto w : layout->colWidths) totalTableWidth += w;
  const int tableX = x + (viewportWidth - totalTableWidth) / 2;

  // Draw top border for first row
  if (isFirstRow) {
    renderer.drawLine(tableX, y, tableX + totalTableWidth, y, true);
  }

  // Draw each cell
  int cellX = tableX;
  for (int col = 0; col < numCols; col++) {
    const int colW = layout->colWidths[col];

    // Left vertical border
    renderer.drawLine(cellX, y, cellX, y + rowH, true);

    // Cell text (truncated to fit)
    if (col < static_cast<int>(cellTexts.size()) && !cellTexts[col].empty()) {
      const auto style = (col < static_cast<int>(cellIsHeader.size()) && cellIsHeader[col]) ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
      const int maxTextW = colW - pad * 2;
      if (maxTextW > 0) {
        const std::string truncated = renderer.truncatedText(fontId, cellTexts[col].c_str(), maxTextW, style);
        const int textY = y + pad;
        renderer.drawText(fontId, cellX + pad, textY, truncated.c_str(), true, style);
      }
    }

    cellX += colW;
  }

  // Right border of last column
  renderer.drawLine(cellX, y, cellX, y + rowH, true);

  // Bottom border
  renderer.drawLine(tableX, y + rowH, tableX + totalTableWidth, y + rowH, true);
}

bool TableRowBlock::serialize(FsFile& file) const {
  // Cell count
  const auto cellCount = static_cast<uint16_t>(cellTexts.size());
  serialization::writePod(file, cellCount);

  // Cell texts
  for (const auto& text : cellTexts) serialization::writeString(file, text);

  // Cell header flags
  for (uint16_t i = 0; i < cellCount; i++) {
    bool isHeader = (i < cellIsHeader.size()) ? cellIsHeader[i] : false;
    serialization::writePod(file, isHeader);
  }

  // Layout data (stored per-row for self-contained deserialization)
  const auto colCount = static_cast<uint16_t>(layout ? layout->colWidths.size() : 0);
  serialization::writePod(file, colCount);
  if (layout) {
    for (auto w : layout->colWidths) serialization::writePod(file, w);
    serialization::writePod(file, layout->fontId);
    serialization::writePod(file, layout->rowHeight);
    serialization::writePod(file, layout->cellPadding);
  }

  serialization::writePod(file, isFirstRow);
  serialization::writePod(file, isLastRow);

  return true;
}

std::unique_ptr<TableRowBlock> TableRowBlock::deserialize(FsFile& file) {
  uint16_t cellCount;
  serialization::readPod(file, cellCount);
  if (cellCount > 200) {
    LOG_ERR("TRB", "Cell count %u exceeds max", cellCount);
    return nullptr;
  }

  std::vector<std::string> texts(cellCount);
  for (auto& t : texts) serialization::readString(file, t);

  std::vector<bool> headers(cellCount);
  for (uint16_t i = 0; i < cellCount; i++) {
    bool h;
    serialization::readPod(file, h);
    headers[i] = h;
  }

  uint16_t colCount;
  serialization::readPod(file, colCount);
  auto layout = std::make_shared<TableColumnLayout>();
  layout->colWidths.resize(colCount);
  for (auto& w : layout->colWidths) serialization::readPod(file, w);
  serialization::readPod(file, layout->fontId);
  serialization::readPod(file, layout->rowHeight);
  serialization::readPod(file, layout->cellPadding);

  bool firstRow, lastRow;
  serialization::readPod(file, firstRow);
  serialization::readPod(file, lastRow);

  return std::unique_ptr<TableRowBlock>(
      new TableRowBlock(std::move(texts), std::move(headers), std::move(layout), firstRow, lastRow));
}
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 4: コミット**

```bash
git add lib/Epub/Epub/blocks/TableRowBlock.h lib/Epub/Epub/blocks/TableRowBlock.cpp
git commit -m "✨ TableRowBlock: テーブル行ブロックの追加"
```

---

### Task 3: PageTableRow の追加 + Page の serialize/deserialize 更新

**Files:**
- Modify: `lib/Epub/Epub/Page.h:14-58`
- Modify: `lib/Epub/Epub/Page.cpp:6-8,34-37,57-61,109-122`

- [ ] **Step 1: Page.h に TAG_PageTableRow と PageTableRow クラスを追加**

`Page.h` の enum に `TAG_PageTableRow = 3` を追加:

```cpp
enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageTableRow = 3,
};
```

`Page.h` の `PageImage` クラスの後（`class Page` の前）に `PageTableRow` を追加:

```cpp
#include "blocks/TableRowBlock.h"  // ファイル先頭の include に追加

// A row from a table grid
class PageTableRow final : public PageElement {
  std::shared_ptr<TableRowBlock> block;

 public:
  PageTableRow(std::shared_ptr<TableRowBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, int viewportWidth = 0) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageTableRow; }
  static std::unique_ptr<PageTableRow> deserialize(FsFile& file);
};
```

- [ ] **Step 2: Page.cpp に PageTableRow の実装を追加**

`Page.cpp` の `PageImage::deserialize` の後に追加:

```cpp
void PageTableRow::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                          const int viewportWidth) {
  block->render(renderer, xPos + xOffset, yPos + yOffset, viewportWidth);
}

bool PageTableRow::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  return block->serialize(file);
}

std::unique_ptr<PageTableRow> PageTableRow::deserialize(FsFile& file) {
  int16_t xPos, yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  auto tb = TableRowBlock::deserialize(file);
  if (!tb) return nullptr;
  return std::unique_ptr<PageTableRow>(new PageTableRow(std::move(tb), xPos, yPos));
}
```

`Page::deserialize` の tag 分岐（行109-122付近）に `TAG_PageTableRow` を追加:

```cpp
    } else if (tag == TAG_PageTableRow) {
      auto ptr = PageTableRow::deserialize(file);
      page->elements.push_back(std::move(ptr));
    } else {
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 4: コミット**

```bash
git add lib/Epub/Epub/Page.h lib/Epub/Epub/Page.cpp
git commit -m "✨ PageTableRow: テーブル行のページ要素追加"
```

---

### Task 4: ChapterHtmlSlimParser のテーブル収集・グリッド生成

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h:71-73`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:279-340,970-1032`

- [ ] **Step 1: ChapterHtmlSlimParser.h にテーブルバッファ用構造体を追加**

`tableColIndex` の後（行73の後）に追加:

```cpp
  // Table grid buffering
  struct TableCellData {
    std::string text;
    bool isHeader = false;  // true for <th>
  };
  struct TableRowData {
    std::vector<TableCellData> cells;
  };
  std::vector<TableRowData> tableBuffer;
  std::string tableCellTextBuffer;  // accumulates text for current cell
  bool tableCellIsHeader = false;

  void flushTableAsGrid();  // called on </table>
```

`flushTableAsGrid` の宣言は private メソッド群（`makePages()` の近く、行91付近）に追加。

- [ ] **Step 2: startElement のテーブル処理を書き換え (ChapterHtmlSlimParser.cpp:279-340)**

現在のコード（行279〜340、`<table>`, `<tr>`, `<td>`/`<th>` の3ブロック）を以下で**全置換**:

```cpp
  // Special handling for tables: buffer cell data for grid rendering.
  if (strcmp(name, "table") == 0) {
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    // Flush any pending text block before the table
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->tableBuffer.clear();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->tableBuffer.emplace_back();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->tableColIndex += 1;
    self->tableCellTextBuffer.clear();
    self->tableCellIsHeader = (strcmp(name, "th") == 0);
    self->depth += 1;
    return;
  }
```

- [ ] **Step 3: characterData のテーブルセルテキスト収集を更新 (行784-786付近)**

現在の `if (self->tableDepth > 1)` ブロックの後に、テーブルセル内テキスト収集を追加:

```cpp
  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Buffer text for table cell (top-level table)
  if (self->tableDepth == 1) {
    // Append text to cell buffer, collapsing whitespace
    for (int i = 0; i < len; i++) {
      char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      // Skip leading space or consecutive spaces
      if (c == ' ' && (self->tableCellTextBuffer.empty() || self->tableCellTextBuffer.back() == ' ')) {
        continue;
      }
      self->tableCellTextBuffer += c;
    }
    return;
  }
```

- [ ] **Step 4: endElement のテーブル処理を書き換え (行970-1032付近)**

既存のテーブル関連 endElement 処理（`tableDepth > 1` の nested table close、`td`/`th` close、`tr` close、`table` close）を以下で**全置換**:

```cpp
  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, skipped");
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    // Save cell data to current row
    if (!self->tableBuffer.empty()) {
      TableCellData cell;
      // Trim trailing space
      if (!self->tableCellTextBuffer.empty() && self->tableCellTextBuffer.back() == ' ') {
        self->tableCellTextBuffer.pop_back();
      }
      cell.text = std::move(self->tableCellTextBuffer);
      cell.isHeader = self->tableCellIsHeader;
      self->tableBuffer.back().cells.push_back(std::move(cell));
    }
    self->tableCellTextBuffer.clear();
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    // Row complete, nothing else to do
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->flushTableAsGrid();
    self->tableBuffer.clear();
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
  }
```

なお、 `ChapterHtmlSlimParser.h` に追加した `TableCellData` と `TableRowData` 型を使うため、endElement 内の `TableCellData` 参照は `ChapterHtmlSlimParser::TableCellData` として修正不要（同クラス内のため）。

- [ ] **Step 5: flushTableAsGrid() を実装**

`ChapterHtmlSlimParser.cpp` の `makePages()` の前あたりに追加:

```cpp
void ChapterHtmlSlimParser::flushTableAsGrid() {
  if (tableBuffer.empty()) return;

  // Determine max column count
  int maxCols = 0;
  for (const auto& row : tableBuffer) {
    maxCols = std::max(maxCols, static_cast<int>(row.cells.size()));
  }
  if (maxCols == 0) return;

  // Cap to prevent excessive columns on small screen
  static constexpr int MAX_TABLE_COLS = 10;
  if (maxCols > MAX_TABLE_COLS) maxCols = MAX_TABLE_COLS;

  // Use small font for table text
  const int tableFontId = SMALL_FONT_ID;
  const int lineH = renderer.getLineHeight(tableFontId);
  static constexpr int16_t CELL_PADDING = 2;
  const int16_t rowHeight = static_cast<int16_t>(lineH + CELL_PADDING * 2);

  // Measure max text width per column
  std::vector<int> maxColTextWidth(maxCols, 0);
  for (const auto& row : tableBuffer) {
    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      const auto style = row.cells[col].isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int textW = renderer.getTextWidth(tableFontId, row.cells[col].text.c_str(), style);
      maxColTextWidth[col] = std::max(maxColTextWidth[col], textW);
    }
  }

  // Calculate column widths: proportional distribution within viewport
  static constexpr int MIN_COL_WIDTH = 30;
  int totalDesired = 0;
  for (int col = 0; col < maxCols; col++) {
    maxColTextWidth[col] += CELL_PADDING * 2;  // add padding
    maxColTextWidth[col] = std::max(maxColTextWidth[col], MIN_COL_WIDTH);
    totalDesired += maxColTextWidth[col];
  }

  auto layout = std::make_shared<TableColumnLayout>();
  layout->colWidths.resize(maxCols);
  layout->fontId = tableFontId;
  layout->rowHeight = rowHeight;
  layout->cellPadding = CELL_PADDING;

  if (totalDesired <= viewportWidth) {
    // Fits: use desired widths
    for (int col = 0; col < maxCols; col++) {
      layout->colWidths[col] = static_cast<uint16_t>(maxColTextWidth[col]);
    }
  } else {
    // Doesn't fit: proportionally scale down
    for (int col = 0; col < maxCols; col++) {
      layout->colWidths[col] =
          static_cast<uint16_t>(std::max(MIN_COL_WIDTH, maxColTextWidth[col] * viewportWidth / totalDesired));
    }
  }

  // Generate table rows as page elements
  const int numRows = static_cast<int>(tableBuffer.size());
  for (int rowIdx = 0; rowIdx < numRows; rowIdx++) {
    const auto& row = tableBuffer[rowIdx];
    std::vector<std::string> texts(maxCols);
    std::vector<bool> headers(maxCols, false);

    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      texts[col] = row.cells[col].text;
      headers[col] = row.cells[col].isHeader;
    }

    auto block = std::make_shared<TableRowBlock>(std::move(texts), std::move(headers), layout, rowIdx == 0,
                                                 rowIdx == numRows - 1);

    // Add to page using same mechanism as addLineToPage
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    if (currentPageNextY + rowHeight > viewportHeight) {
      completePageFn(std::move(currentPage));
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    currentPage->elements.push_back(std::make_shared<PageTableRow>(block, 0, currentPageNextY));
    currentPageNextY += rowHeight;
  }
}
```

`#include "fontIds.h"` をChapterHtmlSlimParser.cppの先頭のincludes に追加。ただし`fontIds.h`は`src/`にあるので、パスを確認する:

```cpp
#include "../../../../src/fontIds.h"
```

もし相対パスが複雑すぎる場合、`SMALL_FONT_ID` の値 `(1073217904)` をハードコードするか、コンストラクタ経由でtableFontIdを渡す。**推奨: コンストラクタに `tableFontId` パラメータを追加し、EpubReaderActivityから `SMALL_FONT_ID` を渡す。**

ChapterHtmlSlimParser.h のコンストラクタパラメータに追加:

```cpp
const int* headingFontIds = nullptr,
int tableFontId = 0)  // 0 = no table font override
```

メンバ変数に追加:

```cpp
int tableFontId = 0;
```

初期化リストに追加:

```cpp
tableFontId(tableFontId)
```

`flushTableAsGrid` 内で `SMALL_FONT_ID` の代わりに `tableFontId != 0 ? tableFontId : fontId` を使用。

Section::createSectionFile にも `tableFontId` パラメータを追加し、EpubReaderActivity から `SMALL_FONT_ID` を渡す。

- [ ] **Step 6: ビルド確認**

Run: `pio run 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 7: コミット**

```bash
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
git commit -m "👍 テーブルをグリッドレイアウトで表示"
```

---

### Task 5: Section バージョンバンプ + EpubReaderActivity の tableFontId 渡し

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp:14-15`
- Modify: `lib/Epub/Epub/Section.h:38-40`
- Modify: `src/activities/reader/EpubReaderActivity.cpp:634-637,762-765`

- [ ] **Step 1: SECTION_FILE_VERSION を 21 に**

```cpp
// lib/Epub/Epub/Section.cpp 行14-15
// Bump version to 21: adds TableRowBlock serialization format.
constexpr uint8_t SECTION_FILE_VERSION = 21;
```

- [ ] **Step 2: Section::createSectionFile に tableFontId パラメータ追加**

`Section.h` の `createSectionFile` シグネチャ末尾:

```cpp
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                         bool embeddedStyle, uint8_t imageRendering, const std::function<void()>& popupFn = nullptr,
                         const int* headingFontIds = nullptr, int tableFontId = 0);
```

`Section.cpp` の createSectionFile 実装シグネチャにも `int tableFontId` 追加し、ChapterHtmlSlimParser コンストラクタ呼び出しに転送。

- [ ] **Step 3: EpubReaderActivity から SMALL_FONT_ID を渡す**

`src/activities/reader/EpubReaderActivity.cpp` の `createSectionFile` 呼び出し2箇所で `SMALL_FONT_ID` を追加:

メインのsection作成（行634付近）:
```cpp
section->createSectionFile(..., popupFn, headingFontIds, SMALL_FONT_ID)
```

silent indexing（行762付近）:
```cpp
nextSection.createSectionFile(..., nullptr, silentHeadingFontIds, SMALL_FONT_ID)
```

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 5: コミット**

```bash
git add lib/Epub/Epub/Section.cpp lib/Epub/Epub/Section.h \
        lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h \
        lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp \
        src/activities/reader/EpubReaderActivity.cpp
git commit -m "👍 テーブルグリッド: Section VERSION 21 + tableFontId 伝搬"
```

---

### Task 6: 統合テスト（デバイスビルド・フラッシュ・動作確認）

- [ ] **Step 1: クリーンビルド**

Run: `pio run -t clean && pio run 2>&1 | tail -5`
Expected: SUCCESS, RAM ~32%, Flash ~85%

- [ ] **Step 2: デバイスにフラッシュ**

Run: `pio run -t upload 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 3: 動作確認チェックリスト**

テーブルを含むEPUBで以下を確認:
- [ ] テーブルが罫線付きグリッドで表示される
- [ ] `<th>` セルが太字で表示される
- [ ] テキストが長いセルは「…」で切り詰められる
- [ ] テーブルがページ境界をまたぐ場合、正しく分割される
- [ ] テーブル以外の通常テキスト表示に影響がない
- [ ] ネストされたテーブルがクラッシュしない（スキップされる）

- [ ] **Step 4: 最終コミット（必要な修正があれば）**

```bash
git add -A
git commit -m "🐛 テーブルグリッド表示の微調整"
```

---

## Done 判定基準

- [ ] `pio run` がエラー・警告なしで通る
- [ ] テーブルが罫線付きグリッドレイアウトで表示される
- [ ] `<th>` セルが太字で表示される
- [ ] セルテキストが列幅に収まらない場合「…」で切り詰められる
- [ ] テーブルがページ境界を正しくまたぐ
- [ ] ネストされたテーブルでクラッシュしない
- [ ] 通常テキスト・見出し・画像表示に影響がない
- [ ] `git diff` で意図しない変更が含まれていない
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
