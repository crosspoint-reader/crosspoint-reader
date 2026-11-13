#pragma once

#include <Epub/EpubSearch.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class EpubSearchResultsActivity final : public UiListActivity {
 public:
  explicit EpubSearchResultsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string query,
                                     std::vector<EpubSearchResult> results);

  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(results.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void drawChrome() override;
  void drawFooter() override;

  std::string query;
  std::vector<EpubSearchResult> results;
  std::vector<std::string> formattedSnippets;
  std::vector<freeink::ui::ListItem> rowItems;
  void buildRowItems();
};
