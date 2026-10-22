#pragma once

#include <vector>

#include "SongbookFiles.h"
#include "SongbookListActivity.h"

// Songbook entry screen: the setlists in /songbook/setlists, then "All songs".
class SongbookActivity final : public SongbookListActivity {
  std::vector<songbook::SetlistFile> setlists;
  bool hasSongs = false;

  const char* headerTitle() const override;
  void activateIndex(int index) override;

 public:
  explicit SongbookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : SongbookListActivity("Songbook", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
};
