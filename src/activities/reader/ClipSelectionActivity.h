#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class ClipSelectionActivity final : public Activity {
 public:
  struct WordRef {
    int x, y, w, h;
    int pageIdx;  // 0 = current, 1 = next, 2 = page after next
    std::string text;
  };

  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordRef> words,
                        std::string bookTitle, std::string author, std::string chapterTitle, int pageNumber, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  struct Rect {
    int x, y, w, h;
  };

  std::vector<WordRef> words;
  std::string bookTitle;
  std::string author;
  std::string chapterTitle;
  int pageNumber;
  int fontId;

  Section& section;
  int startPageInSection;
  int marginTop;
  int marginLeft;

  uint8_t* savedBuffer = nullptr;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  bool needsPageSwitch = false;
  bool initialRender = true;

  ButtonNavigator buttonNavigator;

  Rect alignedRect(int x, int y, int w, int h) const;
  void switchToPage(int pageIdx);
  void drawHighlights();
  int lineEndForward(int idx) const;
  int lineEndBackward(int idx) const;
};
