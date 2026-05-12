#pragma once
#include <Md.h>
#include <vector>
#include "CrossPointSettings.h"
#include "activities/Activity.h"

class MdReaderActivity final : public Activity {
  // ── Inline style flags ───────────────────────────────────────────────────
  enum Style : uint8_t {
    NORMAL = 0,
    BOLD   = 1 << 0,
    ITALIC = 1 << 1,
    CODE   = 1 << 2,
  };

  // ── Block-level type for a rendered line ─────────────────────────────────
  enum class BlockType : uint8_t {
    Text,       // normal paragraph text
    H1,         // # heading
    H2,         // ## heading
    H3,         // ### heading (H4–H6 also mapped here)
    HRule,      // horizontal rule — text field unused
    Blockquote, // > quoted text
    ListItem,   // - / * / + list item
  };

  // ── One styled span within a display line ────────────────────────────────
  // A DisplayLine holds one or more InlineRuns so that mixed bold/italic/normal
  // text on the same line is rendered correctly.
  struct InlineRun {
    std::string text;
    uint8_t     style = NORMAL;
  };

  // ── A single display line after word-wrap ────────────────────────────────
  struct DisplayLine {
    std::vector<InlineRun> runs;
    BlockType block          = BlockType::Text;
    bool      isFirstInBlock = false;  // true on the first wrapped line of a block
  };

  // ── Parsed representation of one raw source line ─────────────────────────
  struct ParsedBlock {
    BlockType              block;
    std::vector<InlineRun> runs;  // inline-parsed spans, ready for word-wrap
  };

  std::unique_ptr<Md> md;
  int currentPage = 0;
  int totalPages  = 1;
  int pagesUntilFullRefresh = 0;

  std::vector<size_t>      pageOffsets;       // file offset for the start of each page
  std::vector<DisplayLine> currentPageLines;

  int  linesPerPage  = 0;  // computed using H1 (tallest) font height
  int  viewportWidth = 0;
  bool initialized   = false;

  // Cached settings for cache invalidation (font/margin changes require re-indexing)
  int     cachedBodyFontId         = 0;
  int     cachedBoldFontId         = 0;
  int     cachedItalicFontId       = 0;
  int     cachedBoldItalicFontId   = 0;
  int     cachedMonoFontId         = 0;
  int     cachedH1FontId           = 0;
  int     cachedH2FontId           = 0;
  int     cachedH3FontId           = 0;
  uint8_t cachedScreenMargin       = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int     cachedOrientedMarginTop    = 0;
  int     cachedOrientedMarginRight  = 0;
  int     cachedOrientedMarginBottom = 0;
  int     cachedOrientedMarginLeft   = 0;

  // ── Markdown parsing ─────────────────────────────────────────────────────
  static ParsedBlock             parseBlockLine(const std::string& raw);
  static std::vector<InlineRun>  parseInlineRuns(const std::string& s, uint8_t baseStyle = NORMAL);
  // Plain-text width of all runs concatenated — used for word-wrap measurement
  int runsWidth(const std::vector<InlineRun>& runs, BlockType block) const;
  // Returns the correct font ID for a block type + inline style combination.
  // Bold/italic are encoded as distinct font IDs so no style parameter is
  // needed on drawText, avoiding ambiguity with any bool overloads.
  int resolveFontId(BlockType block, uint8_t style) const;
  int resolveIndent(BlockType block) const;

  // Split 'runs' at character offset 'charPos' (in plain-text units).
  // 'head' receives the first charPos characters, 'tail' the remainder
  // (leading space at the split point is consumed).
  static void splitRunsAt(const std::vector<InlineRun>& runs, size_t charPos,
                          std::vector<InlineRun>& head, std::vector<InlineRun>& tail);

  // Plain-text length of all runs concatenated
  static size_t runsLength(const std::vector<InlineRun>& runs);

  // ── Paged layout ─────────────────────────────────────────────────────────
  void initializeReader();
  void buildPageIndex();
  bool loadPageAtOffset(size_t offset, std::vector<DisplayLine>& outLines, size_t& nextOffset);

  // ── Rendering ────────────────────────────────────────────────────────────
  void renderPage();
  void renderStatusBar() const;

  // ── Cache & progress ─────────────────────────────────────────────────────
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Md> md)
      : Activity("MdReader", renderer, mappedInput), md(std::move(md)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
