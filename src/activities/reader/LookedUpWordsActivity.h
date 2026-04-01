#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryLookupController.h"
#include "util/LookupHistory.h"

class LookedUpWordsActivity final : public Activity {
 public:
  explicit LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath)
      : Activity("LookedUpWords", renderer, mappedInput),
        cachePath(std::move(bookCachePath)),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string cachePath;
  std::vector<LookupHistory::Entry> entries;
  int selectedIndex = 0;
  bool deleteConfirmMode = false;

  DictionaryLookupController controller;
  ButtonNavigator buttonNavigator;

  static constexpr unsigned long LONG_PRESS_MS = 600;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Convert UI index (0 = most recent) to 0-based file index (0 = oldest).
  int fileIndexOf(int uiIndex) const { return static_cast<int>(entries.size()) - 1 - uiIndex; }

  static const char* glyphFor(LookupHistory::Status s);
};
