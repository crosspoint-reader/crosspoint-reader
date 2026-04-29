#pragma once
#include <cstddef>
#include <cstdint>
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
  uint32_t textRevision = 0;
  struct WrappedLayoutCache {
    bool valid = false;
    int contentWidth = 0;
    uint32_t textRevision = 0;
    std::vector<WriterWrappedLayout::Line> lines;
  };
  WrappedLayoutCache wrappedLayoutCache;

  bool flushInputBuffer();
  void markTextChanged();
  std::string getLineText(const WriterWrappedLayout::Line& line) const;
  const std::vector<WriterWrappedLayout::Line>& getWrappedLines(int contentWidth);
  int countWords(const std::string& text) const;
  void clearPreferredCursorX();
  void moveCursorLeft();
  void moveCursorRight();
  void moveCursorVertical(int lineDelta);
  int findWrappedCursorLine(const std::vector<WriterWrappedLayout::Line>& lines) const;
  int measureCursorX(const WriterWrappedLayout::Line& line, size_t cursorOffset) const;
  size_t findClosestCursorOffsetOnLine(const WriterWrappedLayout::Line& line, int preferredX) const;
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
