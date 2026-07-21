#pragma once

#include <Epub/Page.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "clippings/ClipTextBuilder.h"

// Button-driven quote selection that keeps only one rendered Page resident.
// A tiny callback can load the following page without coupling this activity
// to Epub/Section or keeping two TextBlock arenas in RAM at once.
class ClipSelectionActivity final : public Activity {
 public:
  static constexpr size_t MAX_VISIBLE_WORDS = 192;
  static constexpr size_t MAX_VISIBLE_TEXT_BYTES = 4096;
  static constexpr size_t MAX_DRAFT_WORDS = 256;

  struct PageLoader {
    void* context = nullptr;
    std::unique_ptr<Page> (*load)(void* context, uint16_t page) = nullptr;

    explicit operator bool() const { return context != nullptr && load != nullptr; }
  };

  explicit ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                                 int fontId, int marginLeft, int marginTop, uint16_t sectionPage,
                                 uint16_t sectionPageCount, uint16_t paragraphIndex, PageLoader pageLoader)
      : Activity("ClipSelection", renderer, mappedInput),
        page_(std::move(page)),
        fontId_(fontId),
        marginLeft_(marginLeft),
        marginTop_(marginTop),
        startPage_(sectionPage),
        currentPage_(sectionPage),
        sectionPageCount_(sectionPageCount),
        paragraphIndex_(paragraphIndex),
        pageLoader_(pageLoader) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct WordVisual {
    uint16_t row = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };

  void extractWords();
  int closestOrderInRow(uint16_t row, int centerX) const;
  void moveHorizontal(int direction);
  void moveVertical(int direction);
  bool selectionBuildsAt(int orderIndex) const;
  bool buildSelectionAt(int orderIndex, ClipTextBuilder::Result& result);
  bool buildSelection(ClipTextBuilder::Result& result);
  bool advanceSelectionPage();
  bool canAdvanceSelectionPage() const;
  bool restorePageAfterFailedAdvance(uint16_t page, int cursor, int anchor);
  void confirmSelection();
  void cancel();
  void drawSelection() const;
  void drawHints() const;

  std::unique_ptr<Page> page_;
  const int fontId_;
  const int marginLeft_;
  const int marginTop_;
  const uint16_t startPage_;
  uint16_t currentPage_;
  const uint16_t sectionPageCount_;
  const uint16_t paragraphIndex_;
  const PageLoader pageLoader_;
  uint32_t pageFingerprint_ = 0;

  int lineHeight_ = 0;
  std::vector<ClipTextBuilder::Word> words_;
  std::vector<WordVisual> visuals_;
  std::vector<uint16_t> readingOrder_;
  int cursorOrder_ = 0;
  int anchorOrder_ = -1;
  uint16_t rowCount_ = 0;
  uint32_t pageTextualWordCount_ = 0;
  bool extractionComplete_ = true;

  // Finalized pages are retained as bounded word metadata only; their Page
  // arenas are released before loading the next page. Nothing is persisted
  // until the user confirms the complete selection.
  std::vector<ClipTextBuilder::Word> committedWords_;
  std::vector<uint16_t> combinedOrder_;
  size_t committedTextBytes_ = 0;

  // A clipping activity may be opened by a long Confirm press. Requiring a
  // fresh press before handling its release prevents an accidental anchor.
  bool confirmPressSeen_ = false;
};
