#pragma once

#include <Epub/Page.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "clippings/ClipTextBuilder.h"

// Single-page, button-driven quote selection. The activity owns the Page so
// the TextBlock arenas backing its visible words remain valid for the whole
// interaction. It deliberately does not know about Epub/Section or storage.
class ClipSelectionActivity final : public Activity {
 public:
  static constexpr size_t MAX_VISIBLE_WORDS = 192;
  static constexpr size_t MAX_VISIBLE_TEXT_BYTES = 4096;

  explicit ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                                 int fontId, int marginLeft, int marginTop, uint16_t sectionPage,
                                 uint16_t sectionPageCount, uint16_t paragraphIndex = UINT16_MAX)
      : Activity("ClipSelection", renderer, mappedInput),
        page_(std::move(page)),
        fontId_(fontId),
        marginLeft_(marginLeft),
        marginTop_(marginTop),
        sectionPage_(sectionPage),
        sectionPageCount_(sectionPageCount),
        paragraphIndex_(paragraphIndex) {}

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
  bool buildSelection(ClipTextBuilder::Result& result) const;
  void confirmSelection();
  void cancel();
  void drawSelection() const;
  void drawHints() const;

  std::unique_ptr<Page> page_;
  const int fontId_;
  const int marginLeft_;
  const int marginTop_;
  const uint16_t sectionPage_;
  const uint16_t sectionPageCount_;
  const uint16_t paragraphIndex_;
  uint32_t pageFingerprint_ = 0;

  int lineHeight_ = 0;
  std::vector<ClipTextBuilder::Word> words_;
  std::vector<WordVisual> visuals_;
  std::vector<uint16_t> readingOrder_;
  int cursorOrder_ = 0;
  int anchorOrder_ = -1;
  uint16_t rowCount_ = 0;

  // A clipping activity may be opened by a long Confirm press. Requiring a
  // fresh press before handling its release prevents an accidental anchor.
  bool confirmPressSeen_ = false;
};
