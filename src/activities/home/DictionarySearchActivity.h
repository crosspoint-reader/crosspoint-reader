#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "util/DictionaryHistory.h"

// Home-screen dictionary search: a "New Search" row that opens the keyboard,
// then the latest found words, each of which looks the word up again. The
// header's history button lists every word in the dictionary history.
class DictionarySearchActivity final : public UiListActivity {
 public:
  explicit DictionarySearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DictionarySearch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr size_t RECENT_WORDS = 10;
  static constexpr int MAX_ROWS = 1 + static_cast<int>(RECENT_WORDS);
  static constexpr freeink::ui::ActionId ACTION_HISTORY = ACTION_USER;

  int listCount() const override { return 1 + static_cast<int>(words.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override { onGoHome(HomeMenuItem::DICTIONARY); }
  // The header carries the history button, drawn in buildScreen.
  void drawChrome() override {}
  static void historyActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Re-reads the recent words; call with the render lock held.
  void reloadRows();
  void openKeyboard();
  void openHistory();
  // reopenHistory returns to the history list after the lookup.
  void lookUp(const std::string& word, bool reopenHistory = false);

  std::vector<std::string> words;
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  bool noDictionary = false;
  unsigned long noDictionaryTime = 0;
};
