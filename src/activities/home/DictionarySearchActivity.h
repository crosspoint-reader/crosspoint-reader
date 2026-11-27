#pragma once

#include <string>

#include "activities/UiListActivity.h"

// Home-screen dictionary search page: a "New Search" row that opens the
// keyboard. The header's history button lists every word looked up before.
class DictionarySearchActivity final : public UiListActivity {
 public:
  explicit DictionarySearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DictionarySearch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr freeink::ui::ActionId ACTION_HISTORY = ACTION_USER;

  int listCount() const override { return 1; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override { onGoHome(HomeMenuItem::DICTIONARY); }
  // The header carries the history button, drawn in buildScreen.
  void drawChrome() override {}
  static void historyActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  void openKeyboard();
  void openHistory();
  // reopenHistory returns to the history list after the lookup.
  void lookUp(const std::string& word, bool reopenHistory = false);

  freeink::ui::ListItem newSearchRow{};
  bool noDictionary = false;
  unsigned long noDictionaryTime = 0;
};
