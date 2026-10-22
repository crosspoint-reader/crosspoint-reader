#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Setlist.h"
#include "SongbookListActivity.h"

// Songs of one setlist in order, titled from each file's metadata.
class SetlistActivity final : public SongbookListActivity {
  // Owned here; the pushed song screen borrows it while this screen is stacked.
  songbook::Setlist setlist;
  const char* problem;
  std::vector<std::string> labels;
  std::vector<std::string> artists;

  const char* headerTitle() const override { return setlist.title.c_str(); }
  void activateIndex(int index) override;

 public:
  // problem, when set, is shown instead of the songs (unreadable or empty setlist).
  SetlistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, songbook::Setlist setlist,
                  const char* problem)
      : SongbookListActivity("SongbookSetlist", renderer, mappedInput), setlist(std::move(setlist)), problem(problem) {}
  void onEnter() override;
  void onExit() override;
};
