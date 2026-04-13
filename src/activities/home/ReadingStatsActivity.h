#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BookStatEntry {
    std::string path;
    std::string title;
    std::string author;
    std::string coverBmpPath;
    uint32_t totalSecondsRead = 0;
    uint8_t lastProgressPercent = 0;
  };

  enum class ViewMode { LIST, DETAIL };

  ButtonNavigator buttonNavigator;
  std::vector<BookStatEntry> books;
  int selectorIndex = 0;
  ViewMode viewMode = ViewMode::LIST;

  void loadBooks();
  void renderList();
  void renderDetail(const BookStatEntry& entry);
};
