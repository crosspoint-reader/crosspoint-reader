#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "activities/UiListActivity.h"

class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  int currentSpineIndex = 0;

  // Windowed row buffers: TOC entries are SD-backed (BookMetadataCache LUT
  // reads), so only the rows around the viewport are materialized. A
  // several-hundred-entry TOC (547 in a large collection) built up front cost
  // ~60KB of labels + ListItems — starving the CJK glyph arena into
  // SD-per-repaint — for rows that were never drawn. The window follows
  // nav.top via itemsWindowFirst (see fui::ListProps); refreshing it also
  // batch-prewarms the window's fallback glyphs, so each page of the list
  // pays one bounded SD pass and repaints stay RAM-only.
  static constexpr int TOC_WINDOW = 24;
  std::string windowLabels[TOC_WINDOW];
  freeink::ui::ListItem windowItems[TOC_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshTocWindow(int start);

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
