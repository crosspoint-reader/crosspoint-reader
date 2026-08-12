#pragma once
#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  int currentSpineIndex = 0;

  // Row buffers, built once in onEnter() (the TOC never changes for the
  // lifetime of this screen) and reused by buildScreen() on every repaint
  // (cursor move, tap flash, ...) instead of re-composing an indent+title
  // string per TOC entry each time.
  std::vector<std::string> tocLabels;
  std::vector<freeink::ui::ListItem> tocRowItems;
  void buildTocRowItems();

  // Total TOC items count
  int listCount() const override { return epub ? epub->getTocItemsCount() : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result and Confirm activates on RELEASE here, and a
  // missing epub swallows everything past Back.
  bool handleButtons() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, int currentSpineIndex);
  void onEnter() override;
};
