#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

class DictionaryHistoryActivity final : public Activity {
 public:
  DictionaryHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionaryHistory", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void refreshEntries();
  void lookupSelected();
  void confirmClear();

  std::vector<std::string> entries_;
  Dictionary dictionary_;
  ButtonNavigator navigator_;
  int selected_ = 0;
  int pageItems_ = 1;
  bool busy_ = false;
  bool error_ = false;
  unsigned long errorAt_ = 0;
};
