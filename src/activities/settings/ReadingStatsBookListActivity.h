#pragma once

#include "ReadingStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Phase-2 sub-screen: scrollable list of all books with recorded reading time.
// Sorted by total time descending so the most-read books are easiest to reach.
// Selecting a row pushes ReadingStatsBookDetailActivity.
class ReadingStatsBookListActivity final : public Activity {
 public:
  explicit ReadingStatsBookListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStatsBookList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Cached pointers into ReadingStatsStore::books, sorted by totalSeconds desc.
  // Rebuilt on onEnter() so the list reflects the latest state even after a
  // session has been recorded between visits.
  std::vector<const BookReadingStats*> sortedBooks;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void rebuildSortedBooks();
};
