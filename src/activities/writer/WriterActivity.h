#pragma once
#include <string>

#include "../Activity.h"
#include "WriterDraftStore.h"

class WriterActivity final : public Activity {
  WriterDraftStore draftStore;
  std::string draftText;    // Current draft file contents
  std::string inputBuffer;  // Text in the input buffer, not yet committed to file
  bool showSaveError = false;

  bool flushInputBuffer();
  std::string getRenderedText() const;
  int countWords(const std::string& text) const;
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
  void loop() override;
  void render(RenderLock&&) override;
};
