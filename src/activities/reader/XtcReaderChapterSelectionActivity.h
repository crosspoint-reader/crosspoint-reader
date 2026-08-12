#pragma once
#include <Xtc.h>

#include <memory>
#include <vector>

#include "activities/UiListActivity.h"

class XtcReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;

  // Row buffer, built once in onEnter() (chapters never change for the
  // lifetime of this screen) and reused by buildScreen() on every repaint
  // instead of rebuilding a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void buildRowItems();

  int listCount() const override { return xtc ? static_cast<int>(xtc->getChapters().size()) : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result and Confirm activates on RELEASE here; a `!xtc`
  // guard consumes the pass after Back so nothing else runs without a book.
  bool handleButtons() override;
  void drawChrome() override;
  int findChapterIndexForPage(uint32_t page) const;

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage);
  void onEnter() override;
};
