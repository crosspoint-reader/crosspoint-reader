#pragma once
#include <cstdint>
#include <string>

#include "activities/Activity.h"

class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int totalBooks = 0;
  int booksWithProgress = 0;
  uint64_t storageBytesUsed = 0;
  std::string currentBook;

  void scanLibrary();
};
