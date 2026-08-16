#pragma once

#include <Epub/FootnoteEntry.h>

#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderFootnotesActivity final : public UiListActivity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes);

 private:
  int listCount() const override { return static_cast<int>(footnotes.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result, and Confirm (or Power) activates on RELEASE here.
  bool handleButtons() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;
  // Hints show Back only (empty list) or Back/Select; no Up/Down labels.
  void drawFooter() override;

  const std::vector<FootnoteEntry>& footnotes;
  // Built once in the constructor (footnotes is fixed for this activity's
  // lifetime, no reload path) and reused by buildScreen() on every repaint
  // instead of rebuilding a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void buildRowItems();
};
