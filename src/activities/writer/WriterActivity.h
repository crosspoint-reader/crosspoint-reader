#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "../Activity.h"
#include "WriterDraftStore.h"
#include "WriterWrappedLayout.h"

class WriterActivity final : public Activity {
  WriterDraftStore draftStore;
  std::string draftText;    // Current draft file contents
  std::string inputBuffer;  // Text in the input buffer, not yet committed to file
  size_t cursorIndex = 0;
  int viewportTopLine = 0;
  int preferredCursorX = 0;
  bool hasPreferredCursorX = false;
  bool showSaveError = false;

  bool flushInputBuffer();
  std::string getRenderedText() const;
  int countWords(const std::string& text) const;
  void clearPreferredCursorX();
  void moveCursorLeft();
  void moveCursorRight();
  void moveCursorVertical(int lineDelta);
  int findWrappedCursorLine(const std::vector<WriterWrappedLayout::Line>& lines, const std::string& renderedText) const;
  int measureCursorX(const WriterWrappedLayout::Line& line, const std::string& renderedText, size_t cursorOffset) const;
  size_t findClosestCursorOffsetOnLine(const WriterWrappedLayout::Line& line, const std::string& renderedText,
                                       int preferredX) const;
  void renderFooter() const;
  struct FooterLayout {
    int top;
    int marginLeft;
    int marginRight;
  };

  FooterLayout getFooterLayout() const;

 public:
  explicit WriterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Writer", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
