#pragma once
#include <cstddef>
#include <string>

#include "../Activity.h"
#include "WriterDraftStore.h"

class WriterActivity final : public Activity {
  WriterDraftStore draftStore;
  std::string draftText;    // Current draft file contents
  std::string inputBuffer;  // Text in the input buffer, not yet committed to file
  size_t cursorIndex = 0;
  int viewportTopLine = 0;
  bool showSaveError = false;

  bool flushInputBuffer();
  std::string getRenderedText() const;
  int countWords(const std::string& text) const;
  void moveCursorLeft();
  void moveCursorRight();
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
