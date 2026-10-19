#pragma once

#include <Epub/Page.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/WordSelectionInput.h"

class ClipSelectionActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Clip, Lookup, DefinitionLookup };

  struct Selection {
    uint16_t firstPage = 0;
    uint16_t lastPage = 0;
    uint16_t firstWord = 0;
    uint16_t lastWord = 0;
    bool valid = false;
  };

  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<std::unique_ptr<Page>> pages, int marginLeft, int marginTop, int initialX = -1,
                        int initialY = -1, Mode mode = Mode::Clip);

  // The definition viewer retains these pages until this child exits.
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::span<const std::unique_ptr<Page>> borrowedPages, int marginLeft, int marginTop,
                        Selection selection, int initialX = -1, int initialY = -1);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t row = 0;
    uint8_t pageOffset = 0;
    int8_t characterSpacing = 0;
    uint16_t pageWordIndex = 0;
    int fontId = 0;  // the word's font; headings and CSS font-size runs differ from the section font
    uint32_t startOffset = UINT32_MAX;
    uint32_t endOffset = UINT32_MAX;
    const char* text = nullptr;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool paragraphStart = false;
    bool isRtl = false;
    bool discretionaryHyphen = false;
  };

  static constexpr size_t MAX_SELECTABLE_WORDS = 240;

  bool extractWords();
  size_t pageCount() const { return borrowedPages.empty() ? pages.size() : borrowedPages.size(); }
  const Page& pageAt(size_t index) const { return *(borrowedPages.empty() ? pages[index] : borrowedPages[index]); }
  bool isLookup() const { return mode != Mode::Clip; }
  int actionAt(int x, int y) const;
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  int dragWordAt(int x, int y) const;
  bool selectionContains(int x, int y) const;
  int nextPageStartIndexForTouchDrag() const;
  bool isWithinCurrentPageEndDwellSlop(int x, int y) const;
  void moveVertical(int direction);
  void selectIndex(int index);
  void moveToPage(int pageOffset);
  void confirmSelection(ClippingResult::Action action = ClippingResult::Action::Clip);
  void loopButtons();
  bool handleButtons(uint8_t buttons);
  Rect handleRect(int index, bool start) const;
  Rect actionRect() const;
  int selectionTop() const;
  int textOffset() const;
  int textXOffset() const;
  void cancel();
  bool buildSelectedText(int first, int last, std::string& text) const;
  void drawSelection() const;
  bool renderIncremental();
  void drawWordHighlight(int index, int firstSelected, int lastSelected, int offsetX, int offset) const;
  void drawWordClean(int index, int offsetX, int offset) const;
  void ditherGapBetween(const WordBox& a, const WordBox& b, int offsetX, int offset) const;
  void clearGapBetween(const WordBox& a, const WordBox& b, int offsetX, int offset) const;
  void prewarmWord(int index) const;

  std::vector<std::unique_ptr<Page>> pages;
  std::span<const std::unique_ptr<Page>> borrowedPages;
  Selection initialSelection;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  const Mode mode;
  OptionPopup actionPopup;
  // ponytail: retain 16 button frames during refresh; increase only if overflow is observed.
  std::array<uint8_t, 16> pendingButtons{};
  size_t pendingButtonCount = 0;
  WordSelectionInput selectionInput;
  int fontId = 0;
  int lineHeight = 0;
  std::unique_ptr<WordBox[]> words;
  size_t wordCount = 0;
  int selected = 0;
  int rangeStart = -1;
  uint8_t currentPageOffset = 0;
  uint16_t rowCount = 0;
  bool touchDragSelecting = false;
  bool ignoreInitialTouch = false;
  int dragOffsetX = 0;
  int dragOffsetY = 0;
  bool touchDragHasMoved = false;
  int touchDragStartX = 0;
  int touchDragStartY = 0;
  int touchDragPageEndIndex = -1;
  unsigned long touchDragPageEndHeldSince = 0;
  int lastRenderedSelected = -1;
  int lastRenderedRangeStart = -1;
  int lastRenderedPageOffset = -1;
  int lastRenderedTextOffset = 0;
  int lastRenderedTextXOffset = 0;
};
