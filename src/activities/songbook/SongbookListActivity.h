#pragma once

#include <vector>

#include "activities/UiListActivity.h"

// List screen shared by the setlist picker and a setlist's song list: rows are
// built once on entry; emptyMessage replaces the list when there are none.
class SongbookListActivity : public UiListActivity {
 protected:
  SongbookListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity(name, renderer, mappedInput) {}

  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;

  std::vector<freeink::ui::ListItem> rowItems;
  const char* emptyMessage = nullptr;
};
