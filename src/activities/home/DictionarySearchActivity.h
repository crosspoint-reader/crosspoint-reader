#pragma once

#include <string>

#include "activities/Activity.h"

// Home-screen dictionary search. Alternates the keyboard and a lookup of the
// typed word until the user backs out or a lookup fails.
class DictionarySearchActivity final : public Activity {
 public:
  explicit DictionarySearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySearch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void openKeyboard();
  void lookUp();

  // Prefills the next keyboard so a typo is quick to fix.
  std::string lastWord;
  bool noDictionary = false;
  unsigned long noDictionaryTime = 0;
};
