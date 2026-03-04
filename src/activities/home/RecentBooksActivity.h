#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/BookProgressDataStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  struct PartyBookEntry {
    RecentBook book;
    bool hasProgress = false;
    BookProgressDataStore::ProgressData progress;
    int level = 1;
    std::string progressLabel;
    bool hasPokemon = false;
    std::string pokemonLabel;
    std::string currentFormLabel;
    std::string nextFormLabel;
  };

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool partyMode = false;

  // Recent tab state
  std::vector<PartyBookEntry> recentBooks;

  // Data loading
  void loadRecentBooks();
  bool drawCoverAt(const std::string& coverPath, int x, int y, int width, int height) const;

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
