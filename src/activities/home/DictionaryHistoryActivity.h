#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Every word in the dictionary history, newest first. Picking one finishes
// with a KeyboardResult holding the word; Back finishes cancelled.
class DictionaryHistoryActivity final : public UiListActivity {
 public:
  explicit DictionaryHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DictionaryHistory", renderer, mappedInput) {}

  void onEnter() override;

 private:
  // One placeholder row when the history is empty.
  int listCount() const override { return words.empty() ? 1 : static_cast<int>(words.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;

  std::vector<std::string> words;
  std::vector<freeink::ui::ListItem> rowItems;
};
